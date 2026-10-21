"""UltraShape 1.0's refinement (scripts/infer_dit_refine.py) in less memory: the checkpoint mapped, not read whole,
each part to the GPU as it is loaded. Runs in the fh-ultrashape image:

    python /tools/refine.py --ckpt /ckpt/ultrashape_v1.pt --image ref.png --mesh coarse.obj --out refined.glb
"""
import argparse
import gc
import json
import os
import sys

import torch
from omegaconf import OmegaConf
from PIL import Image

sys.path.insert(0, "/ultrashape")
from ultrashape.pipelines import UltraShapePipeline  # noqa: E402
from ultrashape.surface_loaders import SharpEdgeSurfaceLoader  # noqa: E402
from ultrashape.utils import voxelize_from_point  # noqa: E402
from ultrashape.utils.misc import instantiate_from_config  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ckpt", required=True)
    ap.add_argument("--config", default="/ultrashape/configs/infer_dit_refine.yaml")
    ap.add_argument("--image")
    ap.add_argument("--mesh")
    ap.add_argument("--out")
    ap.add_argument("--jobs", help="a json list of {image, mesh, out}: many meshes, the model loaded once")
    ap.add_argument("--steps", type=int, default=30)
    ap.add_argument("--scale", type=float, default=0.99)
    ap.add_argument("--num_latents", type=int, default=32768)
    ap.add_argument("--chunk_size", type=int, default=8000)
    ap.add_argument("--octree_res", type=int, default=512)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--guidance", type=float, default=None)
    a = ap.parse_args()
    dev = torch.device("cuda")

    config = OmegaConf.load(a.config)
    config.model.params.vae_config.params.num_latents = a.num_latents
    weights = torch.load(a.ckpt, map_location="cpu", mmap=True, weights_only=False)
    parts = {}
    for name, cfg in (("vae", config.model.params.vae_config), ("dit", config.model.params.dit_cfg),
                      ("conditioner", config.model.params.conditioner_config)):
        print(f"loading {name}", flush=True)
        # made on the GPU (no copy of it in memory), in half precision but the VAE
        torch.set_default_dtype(torch.float32 if name == "vae" else torch.float16)
        with torch.device(dev):
            model = instantiate_from_config(cfg)
        torch.set_default_dtype(torch.float32)
        model.load_state_dict(weights[name], strict=True)
        model = model.eval()
        parts[name] = model
        del weights[name]
        gc.collect()
    del weights
    gc.collect()
    if hasattr(parts["vae"], "enable_flashvdm_decoder"):
        parts["vae"].enable_flashvdm_decoder()
    pipe = UltraShapePipeline(vae=parts["vae"], model=parts["dit"], conditioner=parts["conditioner"],
                              scheduler=instantiate_from_config(config.model.params.scheduler_cfg),
                              image_processor=instantiate_from_config(config.model.params.image_processor_cfg))

    jobs = json.load(open(a.jobs)) if a.jobs else [{"image": a.image, "mesh": a.mesh, "out": a.out}]
    loader = SharpEdgeSurfaceLoader(num_sharp_points=204800, num_uniform_points=204800)
    kw = {}
    if a.guidance is not None:
        kw["guidance_scale"] = a.guidance
    for job in jobs:
        image = Image.open(job["image"])
        surface = loader(job["mesh"], normalize_scale=a.scale).to(dev, dtype=torch.float16)
        _, voxel_idx = voxelize_from_point(surface[:, :, :3], a.num_latents,
                                           resolution=config.model.params.vae_config.params.voxel_query_res)
        print(f"refining {job['mesh']}", flush=True)
        with torch.no_grad(), torch.autocast(device_type="cuda", dtype=torch.bfloat16):
            mesh, _ = pipe(image=image, voxel_cond=voxel_idx, generator=torch.Generator(dev).manual_seed(a.seed), box_v=1.0,
                           mc_level=0.0, octree_resolution=a.octree_res, num_inference_steps=a.steps,
                           num_chunks=a.chunk_size, **kw)
        os.makedirs(os.path.dirname(job["out"]) or ".", exist_ok=True)
        mesh[0].export(job["out"])
        print(f"wrote {job['out']}", flush=True)
        torch.cuda.empty_cache()


if __name__ == "__main__":
    main()

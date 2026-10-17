"""The AI models of the remaster: PBRify_Remix (CC0, github.com/Kim2091/PBRify_Remix) through spandrel.
Upscaling 4x (DAT2), then normal, roughness and height (SPAN, 1x) from the upscaled colour. Textures that tile are
padded with their own opposite edges so the maps tile too."""
import os

import numpy as np
import torch
from spandrel import ModelLoader

# PBRIFY_MODELS: the release's Models folder, by default ../tools/ai/pbrify/PBRify_Remix-1.7.2/Models beside the repo
MODELS = os.environ.get("PBRIFY_MODELS") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..",
                                                         "tools", "ai", "pbrify", "PBRify_Remix-1.7.2", "Models")
FILES = {
    "upscale": "4x-PBRify-UpscalerV4.safetensors",
    "normal": "1x-PBRify_NormalV3.pth",
    "rough": "1x-PBRify_RoughnessV2.pth",
    "height": "1x-PBRify_Height.pth",
}


class Models:
    def __init__(self, device="cuda", half=True):
        self.device = device
        self.half = half
        self.nets = {}

    def net(self, name):
        if name not in self.nets:
            d = ModelLoader().load_from_file(os.path.join(MODELS, FILES[name]))
            d.model.eval().to(self.device)
            if self.half and d.supports_half:
                d.model.half()
            self.nets[name] = d
        return self.nets[name]

    @torch.inference_mode()
    def run(self, name, rgb, pad=16, wrap=True):
        """rgb: float32 HxWx3 in 0..1. Returns HxWxC (scaled by the model's scale) in 0..1."""
        d = self.net(name)
        h, w, _ = rgb.shape
        mode = "wrap" if wrap else "reflect"
        p = min(pad, h // 2, w // 2)
        x = np.pad(rgb, ((p, p), (p, p), (0, 0)), mode=mode)
        t = torch.from_numpy(np.ascontiguousarray(x.transpose(2, 0, 1)))[None].to(self.device)
        t = t.half() if self.half and d.supports_half else t.float()
        y = d(t)[0].float().clamp(0, 1).cpu().numpy().transpose(1, 2, 0)
        s = d.scale
        return y[p * s:(p + h) * s, p * s:(p + w) * s]

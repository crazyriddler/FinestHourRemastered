"""Material classes for every extracted texture: OpenCLIP ViT-B-32 (LAION-2B) zero-shot against the classes below,
helped by the texture's names. Writes classes.json: id -> class, score.

    python -I classify.py <extract folder>
"""
import json
import os
import sys

import torch
from PIL import Image

# class: prompts. The order of MATERIALS in materials.py follows these names.
CLASSES = {
    "brick": ["a brick wall texture", "red bricks"],
    "stone": ["a stone wall texture", "rough rock surface", "cobblestone pavement"],
    "concrete": ["a concrete wall texture", "cement surface", "plaster wall"],
    "wood": ["wooden planks texture", "wood boards", "wooden crate", "tree bark"],
    "metal": ["painted metal surface", "rusty steel plate", "metal machinery", "tank armor"],
    "fabric": ["military uniform fabric", "cloth texture", "canvas tent", "leather"],
    "skin": ["a human face texture", "skin", "hair"],
    "ground": ["dirt ground texture", "mud", "gravel", "sand", "snow covered ground"],
    "foliage": ["grass texture", "tree leaves", "bushes"],
    "roof": ["roof tiles texture", "slate roof"],
    "glass": ["a glass window", "window panes"],
    "paper": ["a paper document", "a poster", "a map"],
    "atlas": ["a texture atlas of a gun", "an unwrapped texture of a 3d model", "a texture atlas of a vehicle",
              "a texture atlas of soldier equipment"],
    "sky": ["sky with clouds", "a skybox"],
    "effect": ["fire and smoke particles", "an explosion sprite", "a light glow", "muzzle flash"],
    "ui": ["a user interface icon", "a font atlas with letters", "a game hud element", "a button symbol"],
}
# names that settle the class before the picture is looked at
NAME_HINTS = [
    ("ui", ["fend", "hud", "font", "icon", "button", "menu", "loading_", "pad", "logo", "shell"]),
    ("effect", ["smoke", "fire", "spark", "flash", "tracer", "blood", "explos", "glow", "dust", "muzzle"]),
    ("sky", ["sky"]),
    ("skin", ["face", "head", "_hed", "skin", "hair"]),
]


def main():
    import open_clip
    src = sys.argv[1]
    index = json.load(open(os.path.join(src, "index.json")))
    device = "cuda"
    model, _, preprocess = open_clip.create_model_and_transforms("ViT-B-32", pretrained="laion2b_s34b_b79k")
    model = model.to(device).eval()
    tok = open_clip.get_tokenizer("ViT-B-32")
    names = list(CLASSES)
    prompts = [p for n in names for p in CLASSES[n]]
    owner = [n for n in names for _ in CLASSES[n]]
    with torch.no_grad():
        text = model.encode_text(tok(prompts).to(device))
        text /= text.norm(dim=-1, keepdim=True)
    out = {}
    ids = sorted(index)
    batch = []

    def flush():
        imgs = torch.stack([preprocess(Image.open(os.path.join(src, i + ".png")).convert("RGB")) for i in batch]).to(device)
        with torch.no_grad():
            f = model.encode_image(imgs)
            f /= f.norm(dim=-1, keepdim=True)
            sim = (100 * f @ text.T).softmax(dim=-1).cpu()
        for k, i in enumerate(batch):
            score = {}
            for j, n in enumerate(owner):
                score[n] = score.get(n, 0) + float(sim[k, j])
            cls = max(score, key=score.get)
            hint = None
            low = " ".join(index[i]["names"]).lower()
            for c, words in NAME_HINTS:
                if any(w in low for w in words):
                    hint = c
                    break
            out[i] = {"class": hint or cls, "clip": cls, "score": round(score[cls], 3)}
        batch.clear()

    for i in ids:
        batch.append(i)
        if len(batch) == 128:
            flush()
    if batch:
        flush()
    with open(os.path.join(src, "classes.json"), "w") as fp:
        json.dump(out, fp, indent=1, sort_keys=True)
    counts = {}
    for v in out.values():
        counts[v["class"]] = counts.get(v["class"], 0) + 1
    print(sorted(counts.items(), key=lambda x: -x[1]))


if __name__ == "__main__":
    main()

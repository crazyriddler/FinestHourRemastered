"""Material parameters by class (classify.py). The renderer reads them from materials.json:
    pbr      normal, roughness, height and cavity maps are made
    tiles    the texture repeats: the models see it wrapped, so the maps tile
    normal   strength of the normal map's relief in the lighting
    pom      parallax occlusion depth, in texture widths (0: none)
    spec     specular intensity (scaled by 1 - roughness in the map)
    detail   which detail texture to blend in close up (-1: none), see DETAILS
    alpha_test  cut-out edges kept hard after upscaling
"""
MAX_SPEC = 1.0

DETAILS = ["stone", "wood", "fabric", "metal", "ground", "plaster", "skin"]

_BASE = dict(pbr=True, tiles=True, normal=1.0, pom=0.0, spec=0.15, detail=-1, alpha_test=False)

_CLASSES = {
    "brick": dict(normal=1.0, pom=0.035, spec=0.12, detail=0),
    "stone": dict(normal=1.0, pom=0.04, spec=0.12, detail=0),
    "concrete": dict(normal=0.8, pom=0.02, spec=0.10, detail=5),
    "wood": dict(normal=0.8, pom=0.015, spec=0.18, detail=1),
    "metal": dict(normal=0.7, pom=0.008, spec=0.55, detail=3),
    "fabric": dict(normal=0.6, spec=0.06, detail=2),
    "skin": dict(normal=0.35, spec=0.22, tiles=False),
    "ground": dict(normal=1.0, pom=0.045, spec=0.08, detail=4),
    "foliage": dict(normal=0.5, spec=0.12, tiles=False, alpha_test=True),
    "roof": dict(normal=1.0, pom=0.035, spec=0.18, detail=0),
    "glass": dict(normal=0.3, spec=0.8, tiles=False),
    "paper": dict(normal=0.5, spec=0.06, tiles=False),
    "atlas": dict(normal=0.6, spec=0.2, tiles=False),
    "sky": dict(pbr=False, tiles=False),
    "effect": dict(pbr=False, tiles=False),
    "ui": dict(pbr=False, tiles=False),
}


def params(cls):
    p = dict(_BASE)
    p.update(_CLASSES.get(cls, {}))
    return p

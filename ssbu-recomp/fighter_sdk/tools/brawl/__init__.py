"""Readers for the Wii predecessor's fighter files (publicly documented community formats) and writers for the
native model formats. Pure Python, standard library only. Contains no game content.

    arc.py      the archive container (.pac / .pcs) and its LZ77 compression (types 0x10 and 0x11)
    brres.py    the resource directory inside an archive entry
    mdl0.py     models: bones, vertex arrays, display lists -> triangles with bone weights, materials
    tex0.py     textures (+ palettes) -> RGBA8, PNG writer
    chr0.py     bone animations: keyframe tracks -> per-frame transforms
    psa.py      the moveset file: sections, sub-action scripts, hitbox listing
    ssbh.py     native skeleton / mesh / model description / material files: reader and writer
    nutexb.py   native texture file: writer (RGBA8, block-linear layout) and reader of the footer
    reff.py     particle effects: effect list (EFLS), emitters (REFF), textures (REFT) -> PNG / .nutexb + report
    inventory.py, convert_model.py, convert_anim.py   command-line tools

Every tool takes paths as arguments. Run with  python -I  when the input is a downloaded package.
"""

# Map bundles (`.rmfmap`)

A `.rmfmap` file is a plain zip holding a whole map, laid out the way a storage
backend lays out a building:

```
manifest.json          format, version, yaml name, and a path -> entry table
<id>.building.yaml     byte-identical to what the editor writes normally
layers/<level>/x.png   every image at the path the yaml names it by
```

That is the same shape as `<id>/` in an S3 bucket or a local root, so a bundle
unzips into a building folder and a building folder zips into a bundle. A zip
with no `manifest.json` opens too: the one `*.building.yaml` at its root is the
map, and every other entry the yaml refers to is one of its images. Anything
else in the zip is left alone.

Bundles written before this layout put images under `assets/`. They still open,
since the manifest records where each image lives.

`Download map` in the strip under the canvas writes one. Desktop saves it to a
path you pick, the browser downloads it. Opening one keeps it in memory: the
images are never unpacked to disk, and saving repacks the file in place.

New layer images go under `layers/<level>/` unless you spell out a folder
yourself. The Layers panel says when an image sits somewhere else, and leaves it
there: moving one means moving it in the backend too.

Image paths inside the yaml are relative to the yaml's own directory. Opening a
`building.yaml` rewrites any absolute path that points inside that directory
into the relative form, so the map stays movable. A path that points outside is
left alone and flagged in the Layers panel, as is one whose file is missing.

## Getting a map to start from

[RMF Map Generator](https://github.com/lebibi/isaacsim-rmf-map-generator) sweeps
an occupancy grid out of a live Isaac Sim stage and writes what this editor
opens: a `<name>.building.yaml` with its floorplan PNG, or a `.rmfmap` bundle.
It writes `reference_image` mode with fiducials at the swept world corners, so
the floorplan arrives at the right scale and position rather than being aligned
by hand, and the floors of a multi-level stage come out on one scale.

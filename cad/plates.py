"""Sandwich-mount plates for the soundBud boards.

Two flat plates, board bolted between them on standoffs. Sides stay open, so
there are no connector cutouts to get wrong and every AX22 port stays reachable
while prototyping. Enclose it properly once the module layout stops changing.

    pip install cadquery
    python plates.py          # writes STL + STEP into ./out

All dimensions measured off the official STEP (STP_MTX0013), not the spec page.
"""

import cadquery as cq
from pathlib import Path

# ─── measured from the vendor STEP ──────────────────────────────────────────
# Genesis Mini is 55x55, Genesis One is 55x103; both share the 55mm width and
# the same corner hole inset, so one script covers both.

HOLE_INSET = 3.4     # from each edge to hole centre (27.50 - 24.10)
HOLE_D = 3.4         # measured; the product page claims 2.7 — see README
PCB_THICK = 1.51

# Component clearance, also measured: +9.07 above the PCB, -5.59 below.
CLEAR_TOP = 9.1
CLEAR_BOTTOM = 5.6

# ─── things you may want to tune ────────────────────────────────────────────

PLATE_THICK = 3.0    # 3mm is stiff enough in PLA at this span
MARGIN = 1.0         # plate overhang past the PCB edge, per side
VENT_D = 5.0         # airflow + weight saving + it looks intentional

BOARDS = {"mini": 55.0, "one": 103.0}   # board length in Y


def plate(length: float, vents: bool = True) -> cq.Workplane:
    """One plate for a 55 x `length` board. Origin at centre, matching the STEP."""
    w, l = 55.0 + 2 * MARGIN, length + 2 * MARGIN
    holes = [
        (x, y)
        for x in (-27.5 + HOLE_INSET, 27.5 - HOLE_INSET)
        for y in (-length / 2 + HOLE_INSET, length / 2 - HOLE_INSET)
    ]

    p = cq.Workplane("XY").box(w, l, PLATE_THICK)
    p = p.faces(">Z").workplane().pushPoints(holes).hole(HOLE_D)

    if vents:
        # Grid of vents, kept clear of the mounting holes and the plate edge.
        step = 11.0
        pts = [
            (x, y)
            for x in range(int(-w / 2 + step), int(w / 2), int(step))
            for y in range(int(-l / 2 + step), int(l / 2), int(step))
            if all((x - hx) ** 2 + (y - hy) ** 2 > (HOLE_D + VENT_D) ** 2
                   for hx, hy in holes)
        ]
        if pts:
            p = p.faces(">Z").workplane().pushPoints(pts).hole(VENT_D)

    return p.edges("|Z").fillet(3.0)


def main() -> None:
    out = Path(__file__).parent / "out"
    out.mkdir(exist_ok=True)

    for name, length in BOARDS.items():
        for face, vents in (("bottom", True), ("top", True)):
            part = plate(length, vents)
            stem = f"{name}_{face}"
            cq.exporters.export(part, str(out / f"{stem}.stl"))
            cq.exporters.export(part, str(out / f"{stem}.step"))
        print(f"{name}: 55.0 x {length} board -> "
              f"{55 + 2 * MARGIN:.0f} x {length + 2 * MARGIN:.0f} plates")

    print(f"\nwrote {len(list(out.glob('*')))} files to {out}")
    print(f"standoffs: >={CLEAR_BOTTOM + 1:.0f}mm under the board, "
          f">={CLEAR_TOP + 1:.0f}mm over it")


if __name__ == "__main__":
    main()

    # Smallest check that fails if the geometry breaks: plate is the size we
    # asked for, and the holes actually went through.
    solid = plate(55.0, vents=False).val()
    bb = solid.BoundingBox()
    assert abs(bb.xlen - 57.0) < 0.01, bb.xlen
    assert abs(bb.ylen - 57.0) < 0.01, bb.ylen
    assert abs(bb.zlen - PLATE_THICK) < 0.01, bb.zlen
    # 4 mounting holes -> 4 cylindrical faces
    cyls = [f for f in solid.Faces() if f.geomType() == "CYLINDER"]
    assert len(cyls) == 4 + 4, f"expected 4 holes + 4 fillets, got {len(cyls)}"
    print("ok")

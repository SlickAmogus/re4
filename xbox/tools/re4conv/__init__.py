"""re4conv — offline asset converter for the RE4 Xbox port (xbox/docs/04-DATA.md, CT-04).

Turns the extracted retail GameCube disc trees into the little-endian asset tree the port loads:
same struct layout, every multi-byte field byte-swapped, textures re-encoded for the NV2A, yz2
bodies decompressed, RELs dropped. Reuses the decomp's parsers in tools/ and tools/motion/
(drs.py, archive.py, yz2.py, fcv.py, gxtex.py) which are put on sys.path by `_decomp_tools()`.

Modules: container (drs/das/dat/evd/ETM), model (BIN), tpl (TPL), motion (FCV/SEQ), simple
(fixed-layout tables), adpcm (sound banks, another task). Each exposes convert(bytes) -> bytes
and check(bytes) -> list of problems (empty = ok). See FORMATS.md for status per format.
"""
import os
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))


def _decomp_tools():
    """Puts the decomp's tools/ and tools/motion/ on sys.path (idempotent)."""
    for sub in ('tools', os.path.join('tools', 'motion')):
        p = os.path.join(REPO, sub)
        if p not in sys.path:
            sys.path.insert(0, p)


_decomp_tools()


class ConvertError(ValueError):
    """A structure the converter refuses to emit (bad magic, out-of-range offset, ...)."""

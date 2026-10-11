"""TPL texture palettes -> little-endian headers + ConvertedImage blobs (RE4X).  STUB."""
from . import ConvertError


def convert(d: bytes) -> bytes:
    raise ConvertError('TPL: converter not implemented yet')


def check(d: bytes, original: bytes = None) -> list:
    return ['TPL: checker not implemented yet']

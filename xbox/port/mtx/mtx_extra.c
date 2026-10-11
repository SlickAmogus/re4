/* mtx_extra.c — SDK matrix routines that exist only as paired-single assembly on the GameCube.
 * PSMTXReorder(Mtx src, ROMtx dst): a 3x4 row-major Mtx into the "reordered" column-major 4x3 ROMtx
 * the skinning palette uses (trans.cpp: 0x30 bytes per entry; CalcSk1_x reads it, see port/gx/skin). */
typedef float Mtx[3][4];
typedef float ROMtx[4][3];

void PSMTXReorder(const Mtx src, ROMtx dst)
{
    int r, c;
    for (c = 0; c < 4; c++) {
        for (r = 0; r < 3; r++) {
            dst[c][r] = src[r][c];
        }
    }
}

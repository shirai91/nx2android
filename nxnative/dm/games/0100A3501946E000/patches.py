# OCTOPATH TRAVELER II 1.1.1: build-keyed allocator replacement for the existing host.
# Reference: seedds/nxnative 16f4889192dd9bc6363a77b5d8b9654fe65441a2,
# dm/games/0100A3501946E000/patches.py. These original bytes are checked by the rewriter.
# GCreateMalloc creates a 0x1c38-byte object and calls FMallocBinned3's constructor.
# Skip that constructor; OBS_UE_MALLOC installs the host allocator through x19 before use.
# ue_malloc.tsv provides the matching vtable and constructor site. The 2 GB engine
# region in game.tsv cannot hold the original allocator's virtual-address pools.
CUTS = {
    '0d9649011312f62e93f6a5673a09d7e9': [
        (0xbecc08, 'dc820194', '1f2003d5', 'GCreateMalloc: bl 0xc4d778 (FMallocBinned3 constructor) -> nop; the following gate installs the host allocator'),
    ],
}
OBS = {
    '0d9649011312f62e93f6a5673a09d7e9': [
        (0xbecc0c, 21, 'aa1303e0', 'GCreateMalloc: OBS_UE_MALLOC takes the new allocator from x19; original mov x0, x19'),
    ],
}

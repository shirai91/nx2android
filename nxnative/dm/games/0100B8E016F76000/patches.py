# NieR:Automata The End of YoRHa Edition (Asia 1.0.2, main 992787e2b54259948b844059a8ab21ad).
# CUTS as described in dm/tools/patch_nso.py ("Registered patches"):
# 60 fps frame-cost thresholds, default present interval (30 -> 60 fps), and 1.0f dynamic-resolution floor.
CUTS = {
    '992787e2b54259948b844059a8ab21ad': [
        (0x9c07a4, '8c46a8528301271e', '8c36a8528301271e', '60 fps: bad frame cost 45 -> 22.5 ms'),
        (0x9c07f4, 'ac4781520c40a872', 'ac4781520c30a872', '60 fps: optimal frame cost 32.01 -> 16.005 ms (load 1)'),
        (0x9c0844, 'ac4781520c40a87283cc201e8001271e', 'ac4781520c30a87283cc201e8001271e', '60 fps: optimal frame cost 32.01 -> 16.005 ms (load 2)'),
        (0x9c0868, '00d0271e', '00d0251e', '60 fps: good frame cost 30 -> 15 ms'),
        (0x591198, '20008052', '00008052', '60 fps: frame interval default 30 -> 60 fps'),
        (0x9bc0d8, '41008052', '21008052', '60 fps: NVN present interval default 30 -> 60 fps'),
        (0x9c0898, 'ac9999528cd9a772', '0c0080520cf0a772', 'sharp resolution: min dynamic res floor -> 1.0f (1)'),
        (0x9c08a4, 'cc648a52aceaa772', '0c0080520cf0a772', 'sharp resolution: min dynamic res floor -> 1.0f (2)'),
    ],
}

# NieR:Automata The End of YoRHa Edition (Global / US 1.0.1 v0 and 1.0.3 v131072).
# CUTS as described in dm/tools/patch_nso.py ("Registered patches"):
# 60 fps frame-cost thresholds, default present interval (30 -> 60 fps), and 1.0f dynamic-resolution floor.
CUTS = {
    # 1.0.1 (v0, base cartridge / NSP)
    '6e3b1d88b6e880332d60ac6e8333815f': [
        (0x9bf3a4, '8c46a8528301271e', '8c36a8528301271e', '60 fps: bad frame cost 45 -> 22.5 ms'),
        (0x9bf3f4, 'ac4781520c40a872', 'ac4781520c30a872', '60 fps: optimal frame cost 32.01 -> 16.005 ms (load 1)'),
        (0x9bf444, 'ac4781520c40a87283cc201e8001271e', 'ac4781520c30a87283cc201e8001271e', '60 fps: optimal frame cost 32.01 -> 16.005 ms (load 2)'),
        (0x9bf468, '00d0271e', '00d0251e', '60 fps: good frame cost 30 -> 15 ms'),
        (0x5911a8, '20008052', '00008052', '60 fps: frame interval default 30 -> 60 fps'),
        (0x9bacd8, '41008052', '21008052', '60 fps: NVN present interval default 30 -> 60 fps'),
        (0x9bf498, 'ac9999528cd9a772', '0c0080520cf0a772', 'sharp resolution: min dynamic res floor -> 1.0f (1)'),
        (0x9bf4a4, 'cc648a52aceaa772', '0c0080520cf0a772', 'sharp resolution: min dynamic res floor -> 1.0f (2)'),
    ],
    # 1.0.3 (v131072, optional update)
    '3c10ec35c5ef4c3686b4053afb0e58e7': [
        (0x9bf434, '8c46a8528301271e', '8c36a8528301271e', '60 fps: bad frame cost 45 -> 22.5 ms'),
        (0x9bf484, 'ac4781520c40a872', 'ac4781520c30a872', '60 fps: optimal frame cost 32.01 -> 16.005 ms (load 1)'),
        (0x9bf4d4, 'ac4781520c40a87283cc201e8001271e', 'ac4781520c30a87283cc201e8001271e', '60 fps: optimal frame cost 32.01 -> 16.005 ms (load 2)'),
        (0x9bf4f8, '00d0271e', '00d0251e', '60 fps: good frame cost 30 -> 15 ms'),
        (0x591198, '20008052', '00008052', '60 fps: frame interval default 30 -> 60 fps'),
        (0x9bad68, '41008052', '21008052', '60 fps: NVN present interval default 30 -> 60 fps'),
        (0x9bf528, 'ac9999528cd9a772', '0c0080520cf0a772', 'sharp resolution: min dynamic res floor -> 1.0f (1)'),
        (0x9bf534, 'cc648a52aceaa772', '0c0080520cf0a772', 'sharp resolution: min dynamic res floor -> 1.0f (2)'),
    ],
}

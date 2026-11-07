"""Unity 2020.3 compute variants decoded from independently written serialized bytes, without game files."""
import struct, sys, unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'nxnative/dm/gfx/tools'))
import unityfs


COMPUTE_HASH = bytes.fromhex('d0171b849bb064b2424893e7d4809be5')


def words(*values): return struct.pack('<' + 'I' * len(values), *values)


def text(value):
    raw = value.encode('utf-8')
    return words(len(raw)) + raw + bytes(-len(raw) % 4)


def resource(name): return text(name) + text('generated') + struct.pack('<3i', 2, -1, 4)


def kernel(code, local):
    # cb indices, cbs, textures, builtin samplers, input/output buffers, code, local size; no 2022 requirements field.
    return (words(1, 7, 1) + resource('Constants') + words(1) + resource('Texture') + words(1, 3, 2)
            + words(1) + resource('Input') + words(1) + resource('Output')
            + words(len(code)) + code + bytes(-len(code) % 4) + words(3, *local))


def compute_object():
    variants = (text('') + kernel(b'abc', (64, 1, 1)) + text('USE_NORMALS') + kernel(b'12345', (8, 4, 2)))
    parent = text('Skin') + words(2) + variants + words(1) + text('GLOBAL') + words(1) + text('LOCAL')
    param = text('Weights') + words(1, 0x80000004, 2, 3, 4)
    buffers = words(1) + text('Constants') + words(64, 1) + param
    # Two platform records exercise alignment after resourcesResolved and must not merge their kernels.
    platform = words(22, 50, 1) + parent + buffers + b'\1\0\0\0'
    empty_platform = words(4, 30, 0, 0) + bytes(4)
    return text('ComputeFixture') + words(2) + platform + empty_platform


def serialized(data, type_hash=COMPUTE_HASH):
    # SerializedFile format 21: type trees stripped, one class-72 type and one object.
    meta = b'2020.3.12f1\0' + words(38) + b'\0' + words(1)
    meta += struct.pack('<iBh', 72, 0, -1) + type_hash + words(1)
    meta += bytes(-(20 + len(meta)) % 4)
    meta += struct.pack('<qIIi', 300, 0, len(data), 0)
    offset = (20 + len(meta) + 15) & ~15
    return (struct.pack('>4I', len(meta), offset + len(data), 21, offset) + bytes(4) + meta
            + bytes(offset - 20 - len(meta)) + data)


class ComputeVariants(unittest.TestCase):
    def test_stripped_keyword_variants_keep_code_resources_and_metadata(self):
        tree = unityfs.known_tree(72, COMPUTE_HASH)
        data = compute_object()
        reader = unityfs.Reader(data, True)
        obj = unityfs._read(reader, tree)
        self.assertEqual(reader.p, len(data))
        [(cid, pid, decoded)] = unityfs.serialized_objects(serialized(data), {}, {72}, {(72, COMPUTE_HASH): tree})
        self.assertEqual((cid, pid, decoded), (72, 300, obj))
        self.assertEqual(obj['m_Name'], 'ComputeFixture')
        first, second = obj['variants']
        self.assertEqual((first['targetRenderer'], second['targetRenderer'], second['kernels']), (22, 4, []))
        [parent] = first['kernels']
        self.assertEqual((parent['name'], parent['globalKeywords'], parent['localKeywords']), ('Skin', ['GLOBAL'], ['LOCAL']))
        self.assertEqual([(p['first'], p['second']['code'], p['second']['threadGroupSize']) for p in parent['variantMap']],
                         [('', b'abc', [64, 1, 1]), ('USE_NORMALS', b'12345', [8, 4, 2])])
        variant = parent['variantMap'][1]['second']
        self.assertEqual(variant['cbVariantIndices'], [7])
        self.assertEqual(variant['textures'], [{'name': 'Texture', 'generatedName': 'generated', 'bindPoint': 2, 'samplerBindPoint': -1, 'texDimension': 4}])
        self.assertEqual(variant['builtinSamplers'], [{'sampler': 3, 'bindPoint': 2}])
        self.assertEqual(first['constantBuffers'][0]['params'], [{'name': 'Weights', 'type': 1, 'offset': 0x80000004, 'arraySize': 2, 'rowCount': 3, 'colCount': 4}])
        self.assertTrue(first['resourcesResolved'])
        self.assertFalse(second['resourcesResolved'])

    def test_other_hash_cannot_borrow_the_layout(self):
        tree = unityfs.known_tree(72, COMPUTE_HASH)
        with self.assertRaises(unityfs.NoTree):
            list(unityfs.serialized_objects(serialized(compute_object(), bytes(16)), {}, {72}, {(72, COMPUTE_HASH): tree}))


if __name__ == '__main__': unittest.main()

import json, struct
from pathlib import Path

path = Path(r'C:\Users\AldertLake\Documents\Random Projects\Dagor_Assets_Extractor\scratch\dummy_plane_cockpit_anim.glb')
with path.open('rb') as f:
    data = f.read()
assert data[:4] == b'glTF'
length = struct.unpack_from('<I', data, 8)[0]
offset = 12
chunks = []
while offset < length:
    chunk_len, chunk_type = struct.unpack_from('<II', data, offset)
    offset += 8
    chunk = data[offset:offset+chunk_len]
    offset += chunk_len
    chunks.append((chunk_type, chunk))
json_chunk = next(c for t,c in chunks if t == 0x4E4F534A)
bin_chunk = next(c for t,c in chunks if t == 0x004E4942)
obj = json.loads(json_chunk.decode('utf-8'))

nodes = obj['nodes']
for i,n in enumerate(nodes):
    if n.get('mesh') is not None:
        print('MESHNODE', i, n['name'])

for anim in obj.get('animations', []):
    print('ANIM', anim['name'])
    for ch_i, ch in enumerate(anim['channels']):
        target = ch['target']
        node = target['node']
        name = nodes[node].get('name')
        if name in {'gear_c','wheel_c','prop01_1','flap_r','aileron_l'}:
            sampler = anim['samplers'][ch['sampler']]
            inp = obj['accessors'][sampler['input']]
            out = obj['accessors'][sampler['output']]
            in_bv = obj['bufferViews'][inp['bufferView']]
            out_bv = obj['bufferViews'][out['bufferView']]
            times = struct.unpack_from('<' + 'f'*inp['count'], bin_chunk, in_bv.get('byteOffset',0))
            comp = 4 if out['type']=='VEC4' else 3
            vals = struct.unpack_from('<' + 'f'*(out['count']*comp), bin_chunk, out_bv.get('byteOffset',0))
            first = vals[:comp]
            last = vals[-comp:]
            print('  ', name, target['path'], 'times', times[:3], '...', times[-3:], 'first', first, 'last', last)

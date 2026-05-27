import json, struct
from pathlib import Path
path = Path(r'C:\Users\AldertLake\Documents\Random Projects\Dagor_Assets_Extractor\scratch\dummy_plane_cockpit_anim.glb')
with path.open('rb') as f: data=f.read()
length=struct.unpack_from('<I',data,8)[0]; off=12; chunks=[]
while off<length:
    l,t=struct.unpack_from('<II',data,off); off+=8; chunks.append((t,data[off:off+l])); off+=l
obj=json.loads(next(c for t,c in chunks if t==0x4E4F534A).decode('utf-8')); bin_chunk=next(c for t,c in chunks if t==0x004E4942)
nodes=obj['nodes']
for anim in obj.get('animations', []):
    print('ANIM', anim['name'])
    for ch in anim['channels']:
        node = ch['target']['node']
        if node < 35:
            continue
        name = nodes[node].get('name')
        sampler = anim['samplers'][ch['sampler']]
        out = obj['accessors'][sampler['output']]
        out_bv = obj['bufferViews'][out['bufferView']]
        comp = 4 if out['type']=='VEC4' else 3
        vals = struct.unpack_from('<' + 'f'*(out['count']*comp), bin_chunk, out_bv.get('byteOffset',0))
        first = tuple(round(v,6) for v in vals[:comp])
        last = tuple(round(v,6) for v in vals[-comp:])
        print('  node', node, name, ch['target']['path'], 'first', first, 'last', last)

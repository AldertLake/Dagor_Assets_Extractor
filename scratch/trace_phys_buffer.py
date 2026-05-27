import struct
import json

def trace():
    glb_path = "C:/Users/AldertLake/Documents/DagorAssetsExtractorExports/f_16a_block_10_norway_cockpit/f_16a_block_10_norway_cockpit.glb"
    with open(glb_path, 'rb') as f:
        data = f.read()
    
    l, t = struct.unpack('<II', data[12:20])
    js = json.loads(data[20:20+l].decode('utf-8'))
    
    buffer_views = js['bufferViews']
    
    # Let's filter bufferViews that look like animation bufferViews
    # Animation bufferViews have no target, and their byteOffset is high
    anim_bvs = []
    for idx, bv in enumerate(buffer_views):
        if 'target' not in bv and bv['byteOffset'] > 100000000: # animation curves are at the end
            anim_bvs.append((idx, bv))
            
    # Sort by byteOffset
    anim_bvs.sort(key=lambda x: x[1]['byteOffset'])
    
    print("Animation BufferViews in physical order (by byteOffset):")
    for i in range(min(40, len(anim_bvs))):
        idx, bv = anim_bvs[i]
        print(f"Index in JSON: {idx}, byteOffset: {bv['byteOffset']}, byteLength: {bv['byteLength']}")

if __name__ == '__main__':
    trace()

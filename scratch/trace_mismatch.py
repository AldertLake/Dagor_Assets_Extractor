import struct
import json

def trace():
    glb_path = "C:/Users/AldertLake/Documents/DagorAssetsExtractorExports/f_16a_block_10_norway_cockpit/f_16a_block_10_norway_cockpit.glb"
    with open(glb_path, 'rb') as f:
        data = f.read()
    
    l, t = struct.unpack('<II', data[12:20])
    js = json.loads(data[20:20+l].decode('utf-8'))
    
    for i in range(29780, 29790):
        print(f"bv {i}: {js['bufferViews'][i]}")
        
    print("\nAccessors referencing those bufferViews:")
    for a_idx, acc in enumerate(js['accessors']):
        bv = acc.get('bufferView')
        if bv is not None and 29780 <= bv < 29790:
            print(f"acc {a_idx}: {acc}")

if __name__ == '__main__':
    trace()

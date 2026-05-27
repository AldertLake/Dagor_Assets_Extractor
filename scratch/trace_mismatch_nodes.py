import struct
import json

def trace():
    glb_path = "C:/Users/AldertLake/Documents/DagorAssetsExtractorExports/f_16a_block_10_norway_cockpit/f_16a_block_10_norway_cockpit.glb"
    with open(glb_path, 'rb') as f:
        data = f.read()
    
    l, t = struct.unpack('<II', data[12:20])
    js = json.loads(data[20:20+l].decode('utf-8'))
    
    nodes = js.get('nodes', [])
    for idx, node in enumerate(nodes):
        if 'name' in node:
            print(f"Node {idx}: {node['name']}")

if __name__ == '__main__':
    trace()

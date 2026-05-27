import struct
import json

def trace():
    glb_path = "C:/Users/AldertLake/Documents/DagorAssetsExtractorExports/f_16a_block_10_norway_cockpit/f_16a_block_10_norway_cockpit.glb"
    with open(glb_path, 'rb') as f:
        data = f.read()
    
    l, t = struct.unpack('<II', data[12:20])
    js = json.loads(data[20:20+l].decode('utf-8'))
    
    nodes = js.get('nodes', [])
    animations = js.get('animations', [])
    accessors = js['accessors']
    buffer_views = js['bufferViews']
    
    anim = animations[-1]
    samplers = anim.get('samplers', [])
    channels = anim.get('channels', [])
    
    print("Channel list details (first 30):")
    for i in range(30):
        channel = channels[i]
        sampler_idx = channel['sampler']
        target = channel['target']
        node_idx = target['node']
        node_name = nodes[node_idx].get('name', 'unknown')
        path = target['path']
        
        sampler = samplers[sampler_idx]
        in_acc = accessors[sampler['input']]
        out_acc = accessors[sampler['output']]
        
        in_bv = buffer_views[in_acc['bufferView']]
        out_bv = buffer_views[out_acc['bufferView']]
        
        print(f"Ch {i}: Node {node_idx} ({node_name}), path '{path}'")
        print(f"  Sampler {sampler_idx}: in_acc {sampler['input']} (bv={in_acc['bufferView']} len={in_bv['byteLength']}), out_acc {sampler['output']} (bv={out_acc['bufferView']} len={out_bv['byteLength']} type={out_acc['type']})")

if __name__ == '__main__':
    trace()

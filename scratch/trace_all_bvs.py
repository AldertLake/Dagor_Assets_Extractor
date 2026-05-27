import struct
import json

def trace():
    glb_path = "C:/Users/AldertLake/Documents/DagorAssetsExtractorExports/f_16a_block_10_norway_cockpit/f_16a_block_10_norway_cockpit.glb"
    with open(glb_path, 'rb') as f:
        data = f.read()
    
    l, t = struct.unpack('<II', data[12:20])
    js = json.loads(data[20:20+l].decode('utf-8'))
    
    animations = js.get('animations', [])
    accessors = js['accessors']
    buffer_views = js['bufferViews']
    
    print(f"Total buffer views: {len(buffer_views)}")
    print(f"Total accessors: {len(accessors)}")
    
    anim = animations[-1]
    samplers = anim.get('samplers', [])
    channels = anim.get('channels', [])
    
    print(f"Animation has {len(samplers)} samplers and {len(channels)} channels.")
    
    # Print the first 10 and last 10 samplers/channels with their referenced accessors and buffer views
    print("\n--- FIRST 10 SAMPLERS & CHANNELS ---")
    for i in range(10):
        ch = channels[i]
        samp = samplers[ch['sampler']]
        in_acc_idx = samp['input']
        out_acc_idx = samp['output']
        in_acc = accessors[in_acc_idx]
        out_acc = accessors[out_acc_idx]
        in_bv_idx = in_acc['bufferView']
        out_bv_idx = out_acc['bufferView']
        in_bv = buffer_views[in_bv_idx]
        out_bv = buffer_views[out_bv_idx]
        
        print(f"Ch {i}: Node {ch['target']['node']} path '{ch['target']['path']}'")
        print(f"  Input Acc {in_acc_idx} -> BV {in_bv_idx} (offset={in_bv['byteOffset']}, len={in_bv['byteLength']}), count={in_acc['count']}")
        print(f"  Output Acc {out_acc_idx} -> BV {out_bv_idx} (offset={out_bv['byteOffset']}, len={out_bv['byteLength']}), count={out_acc['count']} type={out_acc['type']}")

if __name__ == '__main__':
    trace()

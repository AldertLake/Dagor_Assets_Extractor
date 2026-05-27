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
    
    print("Printing first 20 samplers from the last animation clip:")
    anim = animations[-1] # The one with failed accessors
    print(f"Animation Name: {anim.get('name')}")
    samplers = anim.get('samplers', [])
    print(f"Total samplers in this anim: {len(samplers)}")
    
    for s_idx in range(min(20, len(samplers))):
        sampler = samplers[s_idx]
        in_acc_idx = sampler['input']
        out_acc_idx = sampler['output']
        
        in_acc = accessors[in_acc_idx]
        out_acc = accessors[out_acc_idx]
        
        in_bv_idx = in_acc['bufferView']
        out_bv_idx = out_acc['bufferView']
        
        in_bv = buffer_views[in_bv_idx]
        out_bv = buffer_views[out_bv_idx]
        
        print(f"Sampler {s_idx}:")
        print(f"  Input Acc {in_acc_idx} -> BV {in_bv_idx} (len={in_bv['byteLength']}), count={in_acc['count']}, type={in_acc['type']}")
        print(f"  Output Acc {out_acc_idx} -> BV {out_bv_idx} (len={out_bv['byteLength']}), count={out_acc['count']}, type={out_acc['type']}")

if __name__ == '__main__':
    trace()

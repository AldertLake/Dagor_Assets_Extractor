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
    
    print(f"Total buffer views in GLB: {len(buffer_views)}")
    print(f"Total accessors in GLB: {len(accessors)}")
    
    for idx, anim in enumerate(animations):
        samplers = anim.get('samplers', [])
        print(f"\nAnim {idx} ({anim.get('name')}):")
        print(f"  Samplers count: {len(samplers)}")
        if len(samplers) > 0:
            first_s = samplers[0]
            last_s = samplers[-1]
            
            in_acc_f = accessors[first_s['input']]
            out_acc_f = accessors[first_s['output']]
            in_bv_f = buffer_views[in_acc_f['bufferView']]
            out_bv_f = buffer_views[out_acc_f['bufferView']]
            
            in_acc_l = accessors[last_s['input']]
            out_acc_l = accessors[last_s['output']]
            in_bv_l = buffer_views[in_acc_l['bufferView']]
            out_bv_l = buffer_views[out_acc_l['bufferView']]
            
            print(f"  First sampler: in_acc={first_s['input']} (bv={in_acc_f['bufferView']} len={in_bv_f['byteLength']}), out_acc={first_s['output']} (bv={out_acc_f['bufferView']} len={out_bv_f['byteLength']})")
            print(f"  Last sampler: in_acc={last_s['input']} (bv={in_acc_l['bufferView']} len={in_bv_l['byteLength']}), out_acc={last_s['output']} (bv={out_acc_l['bufferView']} len={out_bv_l['byteLength']})")

if __name__ == '__main__':
    trace()

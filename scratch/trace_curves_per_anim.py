import struct
import json

def trace():
    glb_path = "C:/Users/AldertLake/Documents/DagorAssetsExtractorExports/f_16a_block_10_norway_cockpit/f_16a_block_10_norway_cockpit.glb"
    with open(glb_path, 'rb') as f:
        data = f.read()
    
    offset = 12
    json_data = None
    while offset < len(data):
        chunk_len, chunk_type = struct.unpack('<II', data[offset:offset+8])
        chunk_data = data[offset+8:offset+8+chunk_len]
        if chunk_type == 0x4E4F534A: # "JSON"
            json_data = json.loads(chunk_data.decode('utf-8'))
            break
        offset += 8 + chunk_len
        
    if not json_data:
        print("No JSON found.")
        return

    buffer_views = json_data.get('bufferViews', [])
    accessors = json_data.get('accessors', [])
    animations = json_data.get('animations', [])

    anim = animations[-1]
    samplers = anim.get('samplers', [])
    channels = anim.get('channels', [])
    
    print(f"Tracing animation '{anim.get('name')}' with {len(samplers)} samplers:")
    
    for i, sampler in enumerate(samplers):
        ch = [c for c in channels if c['sampler'] == i]
        path = ch[0]['target']['path'] if ch else 'unknown'
        
        in_acc = accessors[sampler['input']]
        out_acc = accessors[sampler['output']]
        
        in_bv = buffer_views[in_acc['bufferView']]
        out_bv = buffer_views[out_acc['bufferView']]
        
        # Verify if type matches bufferView byteLength
        type_size = {"SCALAR": 4, "VEC2": 8, "VEC3": 12, "VEC4": 16}[out_acc['type']]
        expected_len = type_size * out_acc['count']
        actual_len = out_bv['byteLength']
        
        if expected_len != actual_len:
            print(f"Sampler {i}: path={path}, count={out_acc['count']}")
            print(f"  Accessor type={out_acc['type']} -> expected_len={expected_len}")
            print(f"  bufferView index={out_acc['bufferView']} -> actual_len={actual_len}")
            print(f"  Input bufferView index={in_acc['bufferView']} -> actual_len={in_bv['byteLength']}")
            
            # Print physical order info
            # Let's find what physical index this bufferView corresponds to
            break

if __name__ == '__main__':
    trace()

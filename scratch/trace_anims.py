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

    print(f"Total buffer views: {len(buffer_views)}")
    print(f"Total accessors: {len(accessors)}")
    print(f"Total animations: {len(animations)}")

    # Let's look at the first animation
    if animations:
        print("\nFirst animation:")
        anim = animations[0]
        print(f"Name: {anim.get('name')}")
        print(f"Channels count: {len(anim.get('channels', []))}")
        print(f"Samplers count: {len(anim.get('samplers', []))}")
        
        # Let's inspect a few samplers & channels
        for i in range(min(5, len(anim['samplers']))):
            sampler = anim['samplers'][i]
            channel = anim['channels'][i]
            input_acc_idx = sampler['input']
            output_acc_idx = sampler['output']
            
            input_acc = accessors[input_acc_idx]
            output_acc = accessors[output_acc_idx]
            
            input_bv_idx = input_acc.get('bufferView')
            output_bv_idx = output_acc.get('bufferView')
            
            input_bv = buffer_views[input_bv_idx] if input_bv_idx is not None else None
            output_bv = buffer_views[output_bv_idx] if output_bv_idx is not None else None
            
            print(f"  Sampler {i}:")
            print(f"    Channel target path: {channel['target']['path']}")
            print(f"    Input (Time) Accessor {input_acc_idx}: {input_acc}")
            print(f"      Referenced bufferView {input_bv_idx}: {input_bv}")
            print(f"    Output (Value) Accessor {output_acc_idx}: {output_acc}")
            print(f"      Referenced bufferView {output_bv_idx}: {output_bv}")

    # Let's find mismatches where accessor expected type size doesn't match bufferView length
    component_sizes = {5126: 4}
    type_components = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}
    
    mismatches = []
    for idx, acc in enumerate(accessors):
        bv_idx = acc.get('bufferView')
        if bv_idx is None or bv_idx >= len(buffer_views):
            continue
        bv = buffer_views[bv_idx]
        
        comp_type = acc.get('componentType')
        acc_type = acc.get('type')
        count = acc.get('count', 0)
        
        if comp_type in component_sizes and acc_type in type_components:
            elem_size = component_sizes[comp_type] * type_components[acc_type]
            expected_len = elem_size * count
            actual_len = bv.get('byteLength', 0)
            if expected_len > actual_len:
                mismatches.append((idx, acc, bv_idx, bv, expected_len, actual_len))
                
    print(f"\nTotal mismatched accessors: {len(mismatches)}")
    if mismatches:
        print("First 10 mismatches:")
        for idx, acc, bv_idx, bv, exp, act in mismatches[:10]:
            print(f"  Accessor {idx}: type={acc.get('type')}, count={acc.get('count')}, expected={exp}, references bufferView {bv_idx} with actual={act}")

if __name__ == '__main__':
    trace()

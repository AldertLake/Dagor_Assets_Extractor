import struct
import json
import os
import sys

def check_glb(filepath):
    print(f"\nChecking GLB: {filepath}")
    if not os.path.exists(filepath):
        print("File does not exist.")
        return False
    
    with open(filepath, 'rb') as f:
        data = f.read()
    
    if len(data) < 20:
        print("File too short.")
        return False
    
    magic, version, length = struct.unpack('<III', data[:12])
    if magic != 0x46546C67: # "glTF"
        print(f"Invalid magic: {hex(magic)}")
        return False
    
    if length != len(data):
        print(f"Header length ({length}) does not match file size ({len(data)})")
    
    # Parse chunks
    offset = 12
    json_data = None
    bin_data = b""
    
    while offset < len(data):
        if offset + 8 > len(data):
            print("Truncated chunk header.")
            break
        chunk_len, chunk_type = struct.unpack('<II', data[offset:offset+8])
        chunk_data = data[offset+8:offset+8+chunk_len]
        if chunk_type == 0x4E4F534A: # "JSON"
            json_data = json.loads(chunk_data.decode('utf-8'))
        elif chunk_type == 0x004E4942: # "BIN"
            bin_data = chunk_data
        offset += 8 + chunk_len
    
    if json_data is None:
        print("No JSON chunk found.")
        return False
    
    # Get buffer size
    buffers = json_data.get('buffers', [])
    if not buffers:
        print("No buffers defined in JSON.")
        return False
    
    glb_buffer_len = buffers[0].get('byteLength', 0)
    actual_bin_len = len(bin_data)
    print(f"GLB JSON Buffer byteLength: {glb_buffer_len}, Actual BIN chunk size: {actual_bin_len}")
    if glb_buffer_len > actual_bin_len:
        print(f"WARNING: JSON buffer length ({glb_buffer_len}) is greater than actual BIN chunk size ({actual_bin_len}).")
        
    buffer_views = json_data.get('bufferViews', [])
    accessors = json_data.get('accessors', [])
    
    # Type sizes
    component_types = {
        5120: (1, 'b'), # BYTE
        5121: (1, 'B'), # UNSIGNED_BYTE
        5122: (2, 'h'), # SHORT
        5123: (2, 'H'), # UNSIGNED_SHORT
        5125: (4, 'I'), # UNSIGNED_INT
        5126: (4, 'f'), # FLOAT
    }
    
    type_components = {
        "SCALAR": 1,
        "VEC2": 2,
        "VEC3": 3,
        "VEC4": 4,
        "MAT2": 4,
        "MAT3": 9,
        "MAT4": 16,
    }
    
    errors = 0
    
    # Validate buffer views
    for idx, bv in enumerate(buffer_views):
        bv_offset = bv.get('byteOffset', 0)
        bv_len = bv.get('byteLength', 0)
        
        # Check alignment
        if bv_offset % 4 != 0:
            print(f"Error: bufferView {idx} byteOffset {bv_offset} is not 4-byte aligned.")
            errors += 1
            
        if bv_offset + bv_len > glb_buffer_len:
            print(f"Error: bufferView {idx} (offset={bv_offset}, len={bv_len}) exceeds buffer length {glb_buffer_len}.")
            errors += 1
            
    # Validate accessors
    for idx, acc in enumerate(accessors):
        bv_idx = acc.get('bufferView')
        if bv_idx is None:
            # Sparse or empty accessor without buffer view
            continue
            
        if bv_idx < 0 or bv_idx >= len(buffer_views):
            print(f"Error: Accessor {idx} references out-of-bounds bufferView {bv_idx}.")
            errors += 1
            continue
            
        bv = buffer_views[bv_idx]
        bv_offset = bv.get('byteOffset', 0)
        bv_len = bv.get('byteLength', 0)
        bv_stride = bv.get('byteStride')
        
        acc_offset = acc.get('byteOffset', 0)
        comp_type = acc.get('componentType')
        count = acc.get('count', 0)
        acc_type = acc.get('type')
        
        if comp_type not in component_types:
            print(f"Error: Accessor {idx} has invalid componentType {comp_type}.")
            errors += 1
            continue
            
        if acc_type not in type_components:
            print(f"Error: Accessor {idx} has invalid type {acc_type}.")
            errors += 1
            continue
            
        comp_size, _ = component_types[comp_type]
        num_comps = type_components[acc_type]
        element_size = comp_size * num_comps
        
        # Accessor alignment checks
        total_offset = bv_offset + acc_offset
        if total_offset % comp_size != 0:
            print(f"Error: Accessor {idx} total offset {total_offset} ({bv_offset} + {acc_offset}) is not aligned to component size {comp_size}.")
            errors += 1
            
        # Bounds checking
        if bv_stride is not None:
            # Strided buffer view
            if count > 0:
                required_len = acc_offset + (count - 1) * bv_stride + element_size
            else:
                required_len = acc_offset
        else:
            # Tightly packed
            required_len = acc_offset + count * element_size
            
        if required_len > bv_len:
            print(f"Error: Accessor {idx} (type={acc_type}, count={count}, offset={acc_offset}, elem_size={element_size}, stride={bv_stride}) requires {required_len} bytes, but bufferView {bv_idx} length is only {bv_len}.")
            errors += 1
            
    if errors == 0:
        print("All checks passed! No out-of-bounds accessors detected.")
        return True
    else:
        print(f"Validation failed with {errors} errors.")
        return False

if __name__ == '__main__':
    files = [
        "scratch/dummy_plane_cockpit.glb",
        "scratch/dummy_plane_cockpit_anim.glb",
        "scratch/dummy_plane_cockpit_norig.glb",
        "scratch/dummy_plane_cockpit_rig.glb"
    ]
    if len(sys.argv) > 1:
        files = sys.argv[1:]
    for f in files:
        check_glb(f)

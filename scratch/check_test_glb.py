import os
import glob
import struct
import json

def trace():
    temp_dir = os.environ.get('TEMP', 'C:/Users/AldertLake/AppData/Local/Temp')
    pattern = os.path.join(temp_dir, 'dagor_validation_*')
    dirs = sorted(glob.glob(pattern), key=os.path.getmtime)
    if not dirs:
        print("No validation directories found.")
        return
        
    latest_dir = dirs[-1]
    print(f"Latest validation directory: {latest_dir}")
    
    glb_files = glob.glob(os.path.join(latest_dir, '*.glb'))
    for glb_path in glb_files:
        print(f"\n--- Checking {os.path.basename(glb_path)} ---")
        with open(glb_path, 'rb') as f:
            data = f.read()
        
        l, t = struct.unpack('<II', data[12:20])
        js = json.loads(data[20:20+l].decode('utf-8'))
        
        animations = js.get('animations', [])
        accessors = js['accessors']
        buffer_views = js['bufferViews']
        
        print(f"Total buffer views: {len(buffer_views)}")
        print(f"Total accessors: {len(accessors)}")
        
        for anim_idx, anim in enumerate(animations):
            print(f"Anim {anim_idx} ({anim.get('name')}):")
            samplers = anim.get('samplers', [])
            channels = anim.get('channels', [])
            for i, ch in enumerate(channels):
                samp = samplers[ch['sampler']]
                in_acc = accessors[samp['input']]
                out_acc = accessors[samp['output']]
                print(f"  Ch {i}: path '{ch['target']['path']}'")
                print(f"    in_acc={samp['input']} -> BV {in_acc['bufferView']} (len={buffer_views[in_acc['bufferView']]['byteLength']})")
                print(f"    out_acc={samp['output']} -> BV {out_acc['bufferView']} (len={buffer_views[out_acc['bufferView']]['byteLength']})")

if __name__ == '__main__':
    trace()

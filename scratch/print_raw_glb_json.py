import struct
import json

def trace():
    glb_path = "C:/Users/AldertLake/Documents/DagorAssetsExtractorExports/f_16a_block_10_norway_cockpit/f_16a_block_10_norway_cockpit.glb"
    with open(glb_path, 'rb') as f:
        data = f.read()
    
    l, t = struct.unpack('<II', data[12:20])
    js = json.loads(data[20:20+l].decode('utf-8'))
    
    print("--- BUFFER VIEWS 28148 - 28160 ---")
    for i in range(28148, 28160):
        print(f"BV {i}: {js['bufferViews'][i]}")
        
    print("\n--- ACCESSORS 41850 - 41862 ---")
    for i in range(41850, 41862):
        print(f"Acc {i}: {js['accessors'][i]}")
        
    print("\n--- ANIMATION SAMPLERS & CHANNELS ---")
    anim = js['animations'][-1]
    for i in range(5):
        print(f"Sampler {i}: {anim['samplers'][i]}")
        print(f"Channel {i}: {anim['channels'][i]}")

if __name__ == '__main__':
    trace()

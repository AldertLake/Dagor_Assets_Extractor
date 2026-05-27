import os
import time

glb_path = "C:/Users/AldertLake/Documents/DagorAssetsExtractorExports/f_16a_block_10_norway_cockpit/f_16a_block_10_norway_cockpit.glb"
if os.path.exists(glb_path):
    mtime = os.path.getmtime(glb_path)
    print(f"File modification time: {time.ctime(mtime)}")
else:
    print("File does not exist.")

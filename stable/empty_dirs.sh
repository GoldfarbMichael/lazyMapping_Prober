#!/bin/bash

# filepath: /home/ubu/Desktop/Michael/lazyMapping_Prober/empty_dirs.sh

DIRS=(
    "data/chrome_clock/1024C_15TST_DynamicSST"
    # "data/1024C_15TST_DynamicSST"
    # "data/512C_15TST_DynamicSST"
    # "data/256C_15TST_DynamicSST"
    # "data/64C_15TST_DynamicSST"
    # "data/1C_15TST_DynamicSST"

)

for dir in "${DIRS[@]}"; do
    if [ -d "$dir" ]; then
        echo "Processing: $dir"
        # Find all subdirectories and empty their contents
        find "$dir" -mindepth 1 -type f -delete
        echo "Emptied contents of $dir"
    else
        echo "Directory not found: $dir"
    fi
done

echo "Done!"
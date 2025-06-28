#!/bin/bash

SOURCE_DIR="textVersion/network/shared"
DEST_DIR="src/common"

# Check if source directory exists
if [[ ! -d "$SOURCE_DIR" ]]; then
    echo "Error: $SOURCE_DIR does not exist"
    exit 1
fi

# Function to get the latest modification time in a directory recursively
get_latest_mtime() {
    local dir="$1"
    if [[ ! -d "$dir" ]]; then
        echo "0"
        return
    fi
    find "$dir" -type f -exec stat -f "%m" {} \; 2>/dev/null | sort -n | tail -1
}

# Function to copy directory
copy_directory() {
    local src="$1"
    local dest="$2"
    local direction="$3"
    
    echo "Copying $src to $dest ($direction)"
    
    # Create destination directory if it doesn't exist
    mkdir -p "$dest"
    
    # Copy all files and subdirectories
    cp -R "$src/"* "$dest/"
    
    echo "Successfully copied $src to $dest"
}

# If destination doesn't exist, copy from source
if [[ ! -d "$DEST_DIR" ]]; then
    echo "$DEST_DIR does not exist, copying from $SOURCE_DIR"
    copy_directory "$SOURCE_DIR" "$DEST_DIR" "source to destination"
    exit 0
fi

# Get latest modification times for both directories
SOURCE_MTIME=$(get_latest_mtime "$SOURCE_DIR")
DEST_MTIME=$(get_latest_mtime "$DEST_DIR")

# Compare modification times and copy accordingly
if [[ "$SOURCE_MTIME" -gt "$DEST_MTIME" ]]; then
    echo "$SOURCE_DIR has newer files than $DEST_DIR"
    copy_directory "$SOURCE_DIR" "$DEST_DIR" "source to destination"
elif [[ "$DEST_MTIME" -gt "$SOURCE_MTIME" ]]; then
    echo "$DEST_DIR has newer files than $SOURCE_DIR"
    copy_directory "$DEST_DIR" "$SOURCE_DIR" "destination to source"
else
    echo "Both directories have the same latest modification time - no copy needed"
fi 

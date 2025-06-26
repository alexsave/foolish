#!/bin/bash

SOURCE_FILE="textVersion/network/common.ts"
DEST_FILE="src/common/common.ts"

# Check if both files exist
if [[ ! -f "$SOURCE_FILE" ]]; then
    echo "Error: $SOURCE_FILE does not exist"
    exit 1
fi

if [[ ! -f "$DEST_FILE" ]]; then
    echo "$DEST_FILE does not exist, copying from $SOURCE_FILE"
    cp "$SOURCE_FILE" "$DEST_FILE"
    echo "Copied $SOURCE_FILE to $DEST_FILE"
    exit 0
fi

# Compare modification times
if [[ "$SOURCE_FILE" -nt "$DEST_FILE" ]]; then
    echo "$SOURCE_FILE is newer than $DEST_FILE"
    cp "$SOURCE_FILE" "$DEST_FILE"
    echo "Copied $SOURCE_FILE to $DEST_FILE"
elif [[ "$DEST_FILE" -nt "$SOURCE_FILE" ]]; then
    echo "$DEST_FILE is newer than $SOURCE_FILE"
    cp "$DEST_FILE" "$SOURCE_FILE"
    echo "Copied $DEST_FILE to $SOURCE_FILE"
else
    echo "Both files have the same modification time - no copy needed"
fi 
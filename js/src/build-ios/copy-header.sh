#!/bin/sh

src_dir="dist/include/"
dst_dir=$1

# Walk through a folder
function foo ()
{
    dir=$1
    for entry in $dir
    do
        if [ -d "$entry" ] ; then
            foo "$entry/*"
        else
            if [ -f "$entry" ]; then
                file="`readlink $entry`"
                if [ ! "$entry" ]; then
                    file="$entry"
                fi
                cp "$file" "$dst_dir/$entry"
            fi
        fi
    done
}

cd $src_dir

foo "*"

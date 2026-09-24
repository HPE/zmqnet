#!/bin/bash

# link subject to change over time...
pandoc_link="https://arti.hpc.amslabs.hpecorp.net:443/artifactory/pe-internal-third-party-generic-local/pandoc/pandoc-2.14.2-linux-$(uname -m).tar.gz"
pandoc_archive=$(basename $pandoc_link)
pandoc_prefix="$PWD/pandoc"
pandoc_binary="$pandoc_prefix/bin/pandoc"

get_pandoc() {

    if [ ! -f "$pandoc_binary" ]; then

	if [ ! -f "$pandoc_archive" ]; then
	    echo "Downloading pandoc from $pandoc_link"
	    wget $pandoc_link -O $pandoc_archive
	fi

	echo "Extracting pandoc into $pandoc_prefix"
	mkdir -p pandoc
	tar xvzf $pandoc_archive --strip-components 1 -C $pandoc_prefix
	rm $pandoc_archive
    fi
}

if [ -n $(command -v pandoc) ]; then
    pandoc_binary="pandoc"
else    
    get_pandoc
fi

for markdown_file in "$@"; do
	manpage_file="${markdown_file%.*}"
	echo "Generating $manpage_file for $markdown_file..."
	$pandoc_binary --standalone -r markdown -w man -o $manpage_file $markdown_file
done
echo "Done."

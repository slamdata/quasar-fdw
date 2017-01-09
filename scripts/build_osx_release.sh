function build()
{
    echo "Building for pg $1"
    brew link --force --overwrite postgresql-$1
    make clean tar
}

build 9.4
build 9.5

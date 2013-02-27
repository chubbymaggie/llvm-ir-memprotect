#!/bin/sh

#set -x

current_dir=$(pwd)
temp_dir=$current_dir/run_temp
mkdir -p $temp_dir

if [ -z "$CLAMP_PLUGIN" ]; then
    echo "CLAMP_PLUGIN variable must be set to point the loadable plugin module (absolute path)"
    exit 1;
fi

if [ -r $CLAMP_PLUGIN ]; then
    # make sure that plugin is accessible from run_temp path too
    cd $temp_dir;
    if [ ! -r $CLAMP_PLUGIN ]; then
        echo "CLAMP_PLUGIN $CLAMP_PLUGIN must be absolute path.";
        exit 1;
    fi
else
    echo "CLAMP_PLUGIN $CLAMP_PLUGIN was not found";
    exit 1;
fi


cd $current_dir;

# if test file not given in $1 then find the tests
if [ -z $1 ]; then
    tests=$(ls -1 test_*.c test_*.ll test_*.cl 2> /dev/null);
else
    tests=$1;
fi

function get_run_command {
    file_name=$1
    grep -E "(//|;)\s*RUN:" $file_name | sed -E "s@.+RUN:[* ]*(.+)@\1@"
}

function run_test {
    echo "########################### ------------- Running $1 ..."
    cd $temp_dir;
    TEST_SRC=$current_dir/$1;
    OUT_FILE=$temp_dir/$1;
    test_command=$(get_run_command $TEST_SRC);
    if eval $test_command; then
        echo ">>>>>>>>>> OK";
    else
        echo "TEST FAILED! intermediate files should be found in run_temp directory";
        echo $test_command
        echo ">>>>>>>>>> !!! !!! FAIL";
    fi
}

# cleanup old files and run the tests
rm -f $temp_dir/*
for test_file in $tests;do
    run_test $test_file;
done;
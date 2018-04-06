#!/bin/bash

checked_tests=0 ;
passed_tests=0 ;
dirs_of_failed_tests="" ;
for i in `ls`; do
  if ! test -d $i ; then
    continue ;
  fi
  if test $i == "scripts" ; then
    continue ;
  fi
  if test $i == "include" ; then
    continue ;
  fi
  checked_tests=`echo "$checked_tests + 1" | bc` ;

  cd $i ;

  # Baseline
  make baseline ;
  ./baseline &> output_baseline.txt ;

  # Transformation
  make clean ;
  make ;
  ./parallelized &> output_parallelized.txt ;

  # Check the output ;
  cmp output_baseline.txt output_parallelized.txt &> /dev/null ;
  if test $? -ne 0 ; then
    dirs_of_failed_tests="${i} ${dirs_of_failed_tests}" ;
  else
    passed_tests=`echo "$passed_tests + 1" | bc` ;
  fi

  cd ../ ;
done

# Print the results
echo "Tests passed: ${passed_tests} / ${checked_tests}" ;
if test "${dirs_of_failed_tests}" != "" ; then
  echo "Tests failed: ${dirs_of_failed_tests}" ;
fi

exit 0;

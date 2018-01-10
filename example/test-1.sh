#!/bin/sh

cat test_text.job | ./mq_copy --in --create --time 2 /my_queue &
./mq_copy --out --unlink --time 2 /my_queue

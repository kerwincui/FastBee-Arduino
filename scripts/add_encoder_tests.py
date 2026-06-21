# -*- coding: utf-8 -*-
import io
with io.open('test/test_periph_config.cpp', 'r', encoding='utf-8') as f:
    lines = f.readlines()

# Insert new tests after line 1818
insert_pos = 1818
new_lines = lines[:insert_pos] + [u'    RUN_TEST(test_encoder_read_counter);\n', u'    RUN_TEST(test_encoder_reset_counter);\n'] + lines[insert_pos:]

with io.open('test/test_periph_config.cpp', 'w', encoding='utf-8') as f:
    f.writelines(new_lines)

print("Added encoder read/reset counter tests")

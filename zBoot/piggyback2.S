.section piggy
.global input_data, input_len
input_len:
.int data_end - data_beg
input_data:
data_beg:
.incbin "a.out.gz"
data_end:

.section piggy
.global input_data, input_len
input_len:
.long (input_data_end - input_data)
input_data:
.incbin "a.out.gz"
input_data_end:

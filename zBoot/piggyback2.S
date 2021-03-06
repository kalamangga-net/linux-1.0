.text
.global input_data, input_len
input_data:
.incbin "a.out.gz"
input_len:
.int input_len - input_data

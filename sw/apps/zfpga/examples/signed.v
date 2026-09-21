// zfpga synth: signed arithmetic and multiplication.
module top(input signed [7:0] a, input signed [7:0] b, input [7:0] u, input [3:0] k,
           output signed [11:0] sum, output lt, output ltu, output signed [7:0] sra,
           output [7:0] srl, output signed [15:0] prod, output [11:0] uprod, output [9:0] mc,
           output signed [9:0] mix, output ge_c);
    assign sum = a + b;                         // sign-extended to 12 bits
    assign lt = a < b;                          // signed compare
    assign ltu = $unsigned(a) < $unsigned(b);   // the same bits, unsigned
    assign sra = a >>> k;                       // arithmetic shift, variable amount
    assign srl = a >> 2;                        // logical: a is signed, >> is not
    assign prod = a * b;                        // signed 8x8 -> 16
    assign uprod = u * k;                       // unsigned
    assign mc = u * 10'd3;                      // by a constant
    assign mix = $signed({1'b0, k}) - a;        // signed, from an unsigned
    assign ge_c = a >= -8'sd5;                  // against a signed literal
endmodule

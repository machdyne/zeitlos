// zfpga synth: for loops, combinational and clocked, nested.
module top(input clk, input rst, input [7:0] d, input [2:0] sel, input we,
           output reg [3:0] ones, output reg [7:0] rev, output reg [7:0] sr, output [7:0] rd,
           output reg [5:0] tri_sum);
    integer i, j;
    reg [7:0] regs [0:7];
    always @(*) begin
        ones = 0;
        for (i = 0; i < 8; i = i + 1) ones = ones + d[i];
        for (i = 0; i < 8; i = i + 1) rev[i] = d[7 - i];
        tri_sum = 0;
        for (i = 0; i < 3; i = i + 1)
            for (j = 0; j <= i; j = j + 1)
                tri_sum = tri_sum + {d[i], d[j]};
    end
    always @(posedge clk) begin
        sr <= {sr[6:0], d[0] ^ sr[7]};
        if (rst) begin
            for (i = 0; i < 8; i = i + 1) regs[i] <= i * 3;
        end else if (we) begin
            regs[sel] <= d;
        end
    end
    assign rd = regs[sel] ^ regs[~sel];
endmodule

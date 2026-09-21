module top(input clk, input [9:0] a, output reg [31:0] q, output [31:0] q2);
    reg [31:0] mem [0:1023];
    initial $readmemh("seed.hex", mem);
    always @(posedge clk) q <= mem[a];
    assign q2 = q ^ 32'h5;
endmodule

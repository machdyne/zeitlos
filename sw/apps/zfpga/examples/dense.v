// zfpga router stress design: dense logic, compactly placed.
module top(input clk, input [7:0] a, input [7:0] b, output [7:0] y, output z);
    reg [63:0] sr;
    reg [15:0] acc;
    reg [15:0] prod;
    always @(posedge clk) begin
        sr <= {sr[62:0], a[0] ^ sr[63] ^ sr[62] ^ sr[60] ^ sr[59]};
        acc <= acc + {a, b};
        prod <= a * b;
    end
    assign y = acc[15:8] ^ sr[7:0] ^ prod[15:8] ^ prod[7:0];
    assign z = ^sr;
endmodule

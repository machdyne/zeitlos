// zfpga synth: blocking assignments in a clocked block -- temporaries
// seen at once, and mixed with non-blocking state.
module top(input clk, input rst, input [7:0] a, input [7:0] b, input sel,
           output reg [7:0] acc, output reg [7:0] last_t, output reg [3:0] cnt);
    reg [7:0] t;
    reg [3:0] c2;
    always @(posedge clk) begin
        t = a + b;                  // a temporary, read at once below
        if (sel) t = t ^ 8'h5A;
        acc <= rst ? 8'd0 : acc + t;
        last_t <= t;
        c2 = cnt + 1'b1;            // blocking from non-blocking state
        cnt <= rst ? 4'd0 : (c2 == 4'd10 ? 4'd0 : c2);
    end
endmodule

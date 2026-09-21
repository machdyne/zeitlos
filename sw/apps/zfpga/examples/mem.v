// zfpga synth: a register file and a small FIFO, as flip-flops.
module top(input clk, input rst, input we, input [2:0] wa, input [7:0] wd,
           input [2:0] ra0, input [2:0] ra1, output [7:0] rd0, output reg [7:0] rd1,
           input push, input pop, input [3:0] din, output [3:0] dout, output empty, output full);
    reg [7:0] rf [0:7];
    always @(posedge clk) begin
        if (we) rf[wa] <= wd;
        rd1 <= rf[ra1];
    end
    assign rd0 = rf[ra0];

    reg [3:0] q [3:0];
    reg [1:0] wp, rp;
    reg [2:0] n;
    assign empty = n == 0;
    assign full = n == 4;
    assign dout = q[rp];
    always @(posedge clk)
        if (rst) begin
            wp <= 0; rp <= 0; n <= 0;
        end else begin
            if (push && !full) begin q[wp] <= din; wp <= wp + 1'b1; end
            if (pop && !empty) rp <= rp + 1'b1;
            n <= n + (push && !full) - (pop && !empty);
        end
endmodule

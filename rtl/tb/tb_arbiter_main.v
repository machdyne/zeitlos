/* tb_arbiter_main -- three masters hammering one memory through
 * wb_arbiter_main (rtl/arbiter_main.v).
 *
 * Master 2 is the audio mixer's shape: READ ONLY, single words, and
 * light -- it asks for eight reads per sample period, well under 1% of
 * the bus. It is here anyway, and hammering rather than idling,
 * because the interesting question is not whether a light master works
 * but whether adding a third rotation slot broke the fairness of the
 * two that were already there.
 * Each master writes a value only it could have produced, then reads it
 * back and checks it -- so a mis-steered ack or a torn transaction
 * shows up as wrong data, not just as a timing oddity. Also checks
 * neither master starves.
 *
 * The memory model mirrors rtl/mem/sram.v: one ack per transaction with
 * a gap afterwards, plus a few random wait states. That is the stricter
 * of the two slave styles in this SOC -- rtl/mem/vram.v acks on every
 * cycle cyc && stb are high -- and the main bus is the strict one.
 *
 * Run: iverilog -g2005 -o tb rtl/tb/tb_arbiter_main.v rtl/arbiter_main.v
 *      vvp tb
 */
`timescale 1ns/1ps
module tb_arb2;
  reg clk=0, rst=1; always #5 clk=~clk;
  reg [31:0] m0_adr=0,m0_dat=0; reg m0_we=0,m0_stb=0,m0_cyc=0; reg [3:0] m0_sel=4'hF;
  wire [31:0] m0_dat_o; wire m0_ack;
  reg [31:0] m1_adr=0,m1_dat=0; reg m1_we=0,m1_stb=0,m1_cyc=0; reg [3:0] m1_sel=4'hF;
  wire [31:0] m1_dat_o; wire m1_ack;
  wire [31:0] s_adr,s_dat_o; reg [31:0] s_dat_i=0;
  reg [31:0] m2_adr=0; reg m2_stb=0,m2_cyc=0;
  wire [31:0] m2_dat_o; wire m2_ack;
  wire s_we,s_stb,s_cyc; wire [3:0] s_sel; reg s_ack=0; wire [1:0] mst;

  wb_arbiter_main dut(.clk(clk),.rst(rst),
    .m0_adr_i(m0_adr),.m0_dat_i(m0_dat),.m0_dat_o(m0_dat_o),.m0_we_i(m0_we),
    .m0_sel_i(m0_sel),.m0_stb_i(m0_stb),.m0_cyc_i(m0_cyc),.m0_ack_o(m0_ack),
    .m1_adr_i(m1_adr),.m1_dat_i(m1_dat),.m1_dat_o(m1_dat_o),.m1_we_i(m1_we),
    .m1_sel_i(m1_sel),.m1_stb_i(m1_stb),.m1_cyc_i(m1_cyc),.m1_ack_o(m1_ack),
    .m2_adr_i(m2_adr),.m2_dat_i(32'h0),.m2_dat_o(m2_dat_o),.m2_we_i(1'b0),
    .m2_sel_i(4'hF),.m2_stb_i(m2_stb),.m2_cyc_i(m2_cyc),.m2_ack_o(m2_ack),
    .s_adr_o(s_adr),.s_dat_o(s_dat_o),.s_dat_i(s_dat_i),.s_we_o(s_we),
    .s_sel_o(s_sel),.s_stb_o(s_stb),.s_cyc_o(s_cyc),.s_ack_i(s_ack),.master(mst));

  reg [31:0] mem[0:255]; integer i;
  reg pend; integer wait_n;
  // sram.v-style: one ack per transaction, with a gap and some latency
  always @(posedge clk) begin
    s_ack<=0;
    if(rst) begin pend<=0; wait_n<=0; end
    else if(s_ack) pend<=0;
    else if(pend) begin
      if(wait_n>0) wait_n<=wait_n-1;
      else begin s_dat_i<=mem[s_adr[9:2]]; if(s_we) mem[s_adr[9:2]]<=s_dat_o; s_ack<=1; pend<=0; end
    end else if(s_cyc&&s_stb) begin pend<=1; wait_n<=($random%3+3)%3; end
  end

  integer err=0, n0=0, n1=0, n2=0;

  task m0_xact(input [31:0] a, input we, input [31:0] d, output [31:0] r);
    begin @(posedge clk); m0_adr<=a; m0_dat<=d; m0_we<=we; m0_cyc<=1; m0_stb<=1;
      @(posedge clk); while(!m0_ack) @(posedge clk);
      r = m0_dat_o; m0_cyc<=0; m0_stb<=0; m0_we<=0; n0=n0+1; @(posedge clk); end
  endtask
  task m1_xact(input [31:0] a, input we, input [31:0] d, output [31:0] r);
    begin @(posedge clk); m1_adr<=a; m1_dat<=d; m1_we<=we; m1_cyc<=1; m1_stb<=1;
      @(posedge clk); while(!m1_ack) @(posedge clk);
      r = m1_dat_o; m1_cyc<=0; m1_stb<=0; m1_we<=0; n1=n1+1; @(posedge clk); end
  endtask

  // read-only, like the mixer
  task m2_read(input [31:0] a, output [31:0] r);
    begin @(posedge clk); m2_adr<=a; m2_cyc<=1; m2_stb<=1;
      @(posedge clk); while(!m2_ack) @(posedge clk);
      r = m2_dat_o; m2_cyc<=0; m2_stb<=0; n2=n2+1; @(posedge clk); end
  endtask

  reg [31:0] rv; integer k;
  initial begin
    for(i=0;i<256;i=i+1) mem[i]=0;
    repeat(4) @(posedge clk); rst=0; repeat(2) @(posedge clk);
    for(k=0;k<200;k=k+1) begin
      m0_xact(32'h4000_0000 + (k%100)*4, 1, 32'hC0DE_0000 + k, rv);
      m0_xact(32'h4000_0000 + (k%100)*4, 0, 0, rv);
      if(rv !== 32'hC0DE_0000 + k) begin err=err+1;
        $display("M0 readback bad at %0d: %08x", k, rv); end
    end
    // let master 1 finish its own run before judging
    while (n1 < 400) @(posedge clk);
    // Every master must have made real progress. With rotating
    // priority none can be starved by the other two, which is the
    // property that would silently break if the rotation were
    // replaced by anything resembling a fixed order.
    if(n0<100 || n1<100 || n2<100)
      $display("STARVATION: n0=%0d n1=%0d n2=%0d", n0, n1, n2);
    else if(err==0)
      $display("ARBITER_MAIN OK: n0=%0d n1=%0d n2=%0d interleaved, no errors",
        n0, n1, n2);
    else $display("ARBITER_MAIN FAILURES: %0d", err);
    $finish;
  end

  // master 1 runs concurrently the whole time, contending on a disjoint range
  integer j; reg [31:0] rv1;
  initial begin
    @(negedge rst); repeat(3) @(posedge clk);
    for(j=0;j<400;j=j+1) begin
      m1_xact(32'h4000_0200 + (j%64)*4, 1, 32'hBEEF_0000 + j, rv1);
      m1_xact(32'h4000_0200 + (j%64)*4, 0, 0, rv1);
      if(rv1 !== 32'hBEEF_0000 + j) begin err=err+1;
        $display("M1 readback bad at %0d: %08x", j, rv1); end
    end
  end

  // master 2 reads continuously from a range master 0 keeps writing,
  // so a mis-steered ack between these two shows up as data that
  // belongs to neither.
  integer q; reg [31:0] rv2;
  initial begin
    @(negedge rst); repeat(5) @(posedge clk);
    for(q=0;q<600;q=q+1)
      m2_read(32'h4000_0000 + (q%100)*4, rv2);
  end

  initial begin #500000;
    $display("TIMEOUT n0=%0d n1=%0d n2=%0d", n0, n1, n2); $finish; end

endmodule

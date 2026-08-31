`timescale 1ns/1ps
/*
 * How long does the hardware scroll actually TAKE, against the glyph
 * re-render it replaces?
 *
 * Same trio as tb_system_vscroll.v -- real gpu_blit_wb, real
 * wb_arbiter_vram, real vram_wb -- but this one measures cycles
 * rather than checking pixels. Both figures come from the same
 * simulation, through the same bus, so they are directly comparable;
 * no arithmetic on datasheet numbers.
 *
 * Reported per app-sized region:
 *   - one VRAM-to-VRAM scroll blit of the whole text area
 *   - the glyph blits a full re-render of that area would cost
 *   - the glyph blits ONE ROW costs (what a scrolling app actually
 *     redraws after a blit)
 *
 * The comparison an app integration has to win is not
 * "blit vs full re-render" -- it is "blit + new rows" against
 * "whatever the app would otherwise have drawn", and for an app that
 * only draws what changed, that second number can be very small.
 */
module tb_scroll_timing;
    reg clk=0,rst=1; always #5 clk=~clk;

    reg wb_cyc=0,wb_stb=0,wb_we=0; reg [31:0] wb_adr=0,wb_dat=0;
    wire [31:0] wb_dat_o; wire wb_ack;

    wire m_cyc,m_stb,m_we; wire [3:0] m_sel; wire [31:0] m_adr,m_dat_o;
    wire [31:0] m_dat_i; wire m_ack;
    wire s_cyc,s_stb,s_we; wire [3:0] s_sel; wire [31:0] s_adr;
    wire [11:0] glyph_addr; wire busy;

    /* Glyph ROM stub: every row of every glyph is 8'hA5. Content is
     * irrelevant to timing -- the blitter walks the same states for
     * any bit pattern -- and a stub keeps this bench free of the
     * glyph.v lint fixup the others need. */
    reg [7:0] glyph_data;
    always @(posedge clk) glyph_data <= 8'hA5;

    gpu_blit_wb dut(.clk(clk),.rst(rst),
      .wb_cyc_i(wb_cyc),.wb_stb_i(wb_stb),.wb_we_i(wb_we),.wb_sel_i(4'hF),
      .wb_adr_i(wb_adr),.wb_dat_i(wb_dat),.wb_ack_o(wb_ack),.wb_dat_o(wb_dat_o),
      .m_cyc_o(m_cyc),.m_stb_o(m_stb),.m_we_o(m_we),.m_sel_o(m_sel),
      .m_adr_o(m_adr),.m_dat_o(m_dat_o),.m_dat_i(m_dat_i),.m_ack_i(m_ack),
      .s_cyc_o(s_cyc),.s_stb_o(s_stb),.s_we_o(s_we),.s_sel_o(s_sel),
      .s_adr_o(s_adr),.s_dat_i(32'h0),.s_ack_i(1'b0),
      .glyph_addr_o(glyph_addr),.glyph_data_i(glyph_data),.busy(busy));

    wire [31:0] a_adr,a_dat_o; wire [3:0] a_sel;
    wire a_we,a_stb,a_cyc; wire [31:0] a_dat_i; wire a_ack;
    wire [31:0] nc0,nc1; wire nca0,nca1; wire [1:0] amaster;

    wb_arbiter_vram varb(.clk(clk),.rst(rst),
      .m0_adr_i(32'h0),.m0_dat_i(32'h0),.m0_dat_o(nc0),
      .m0_we_i(1'b0),.m0_sel_i(4'h0),.m0_stb_i(1'b0),.m0_cyc_i(1'b0),.m0_ack_o(nca0),
      .m1_adr_i(32'h0),.m1_dat_i(32'h0),.m1_dat_o(nc1),
      .m1_we_i(1'b0),.m1_sel_i(4'h0),.m1_stb_i(1'b0),.m1_cyc_i(1'b0),.m1_ack_o(nca1),
      .m2_adr_i(m_adr),.m2_dat_i(m_dat_o),.m2_dat_o(m_dat_i),
      .m2_we_i(m_we),.m2_sel_i(m_sel),.m2_stb_i(m_stb),.m2_cyc_i(m_cyc),.m2_ack_o(m_ack),
      .s_adr_o(a_adr),.s_dat_o(a_dat_o),.s_dat_i(a_dat_i),
      .s_we_o(a_we),.s_sel_o(a_sel),.s_stb_o(a_stb),.s_cyc_o(a_cyc),.s_ack_i(a_ack),
      .master(amaster));

    wire [31:0] gb_dat;
    vram_wb vram0(.wb_clk_i(clk),.wb_rst_i(rst),
      .wb_adr_i(a_adr[16:2]),.wb_dat_i(a_dat_o),.wb_dat_o(a_dat_i),
      .wb_we_i(a_we),.wb_sel_i(a_sel),.wb_stb_i(a_stb),.wb_ack_o(a_ack),
      .wb_cyc_i(a_cyc),.gb_adr_i(15'h0),.gb_dat_o(gb_dat));

    localparam VRAM_BASE=32'h20000000;
    integer i;

    /* Cycle counter, and the register-write cost measured with it so
     * the per-operation setup is not silently excluded. */
    integer cyc; always @(posedge clk) cyc = cyc + 1;
    integer t0, t_blit, t_glyph_row, t_glyph_screen, t_setup;

    task wb_write(input [31:0] a,input [31:0] d);
      begin @(posedge clk); wb_adr<=a; wb_dat<=d; wb_we<=1; wb_cyc<=1; wb_stb<=1;
        @(posedge clk); while(!wb_ack) @(posedge clk);
        wb_cyc<=0; wb_stb<=0; wb_we<=0; @(posedge clk); end endtask
    task wait_done; integer g; begin g=0;
      while(!busy && g<80) begin @(posedge clk); g=g+1; end
      while(busy) @(posedge clk); @(posedge clk); end endtask

    localparam R_CTRL=0,R_DX=2,R_DY=3,R_W=4,R_H=5,R_PAT=6,
               R_SA=12,R_SS=13,R_SH=14;
    localparam C_START=32'h1, C_CLIP=32'h4, C_GLYPH=32'h8;

    task blit_vram(input integer sx,input integer sy,
                   input integer dx,input integer dy,
                   input integer w,input integer h);
      integer sbit0, sword, sshift; reg [31:0] prime;
      begin
        sbit0 = sx - (dx % 32);
        if (sbit0 >= 0) begin sword = sbit0/32; sshift = sbit0%32; prime=0; end
        else begin sword = 0; sshift = sbit0+32; prime=32'h100; end
        wb_write(R_DX,dx); wb_write(R_DY,dy); wb_write(R_W,w); wb_write(R_H,h);
        wb_write(R_SA, VRAM_BASE + sy*80 + sword*4);
        wb_write(R_SS, 80);
        wb_write(R_SH, prime | sshift);
        wb_write(R_CTRL, C_START | C_CLIP);
        wait_done();
      end endtask

    /* One character cell, exactly as z_fb_draw_char() programs it. */
    task glyph_at(input integer x,input integer y,
                  input integer w,input integer h);
      begin
        wb_write(R_DX,x); wb_write(R_DY,y);
        wb_write(R_W,w);  wb_write(R_H,h);
        wb_write(R_PAT,32'h0);
        wb_write(R_SH,32'h0);
        wb_write(R_CTRL, C_START | C_GLYPH | C_CLIP);
        wait_done();
      end endtask

    integer cols, rows, fw, fh, x0, y0, wpx, hpx, r, c;

    initial begin
      cyc = 0;
      for(i=0;i<9600;i=i+1) vram0.vram[i] = i*32'h01234567;
      repeat(4) @(posedge clk); rst=0; repeat(2) @(posedge clk);

      /* -- term: 80x25 of z_font_5x8, the case docs/gpu_blitter.md
       * quotes 1.07ms against 4.67ms for. -- */
      fw=5; fh=8; cols=80; rows=25;
      x0=46; y0=53; wpx=cols*fw; hpx=rows*fh;

      /* register-write setup cost alone, for scale */
      t0=cyc; wb_write(R_DX,0); t_setup=cyc-t0;

      t0=cyc;
      blit_vram(x0, y0+fh, x0, y0, wpx, hpx-fh);
      t_blit=cyc-t0;

      t0=cyc;
      for(c=0;c<cols;c=c+1) glyph_at(x0+c*fw, y0, fw, fh);
      t_glyph_row=cyc-t0;

      t_glyph_screen = t_glyph_row*rows;

      $display("");
      $display("=== term: %0dx%0d cells, %0dx%0d font, %0dx%0d px ===",
               cols,rows,fw,fh,wpx,hpx);
      $display("  one register write        : %0d cycles", t_setup);
      $display("  scroll blit (whole area)  : %0d cycles", t_blit);
      $display("  glyph re-render, ONE row  : %0d cycles  (%0d cells)",
               t_glyph_row, cols);
      $display("  glyph re-render, FULL     : %0d cycles  (measured row x %0d)",
               t_glyph_screen, rows);
      $display("  blit + 1 new row          : %0d cycles", t_blit+t_glyph_row);
      $display("");
      $display("  scroll accelerated  : %0d cycles saved vs full re-render",
               t_glyph_screen - (t_blit+t_glyph_row));
      $display("  BREAK-EVEN: the blit only pays if the app would");
      $display("  otherwise redraw more than %0d cells (%0d rows' worth).",
               (t_blit+t_glyph_row)/(t_glyph_row/cols),
               (t_blit+t_glyph_row)/t_glyph_row);
      $display("");

      /* -- read/text: a taller, narrower body area -- */
      fw=5; fh=8; wpx=303; hpx=225;
      t0=cyc;
      blit_vram(46, 53+9, 46, 53, wpx, hpx-9);
      $display("=== read/text body: %0dx%0d px, dy=-9 ===", wpx, hpx);
      $display("  scroll blit               : %0d cycles", cyc-t0);
      $display("  (one 60-cell row of glyphs: %0d cycles)",
               (t_glyph_row/cols)*60);
      $display("");
      $finish;
    end
endmodule

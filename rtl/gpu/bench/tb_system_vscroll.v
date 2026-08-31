`timescale 1ns/1ps
/*
 * SYSTEM-LEVEL vertical scroll test: the real gpu_blit_wb driving the
 * real wb_arbiter_vram driving the real vram_wb -- the trio that runs
 * on hardware. tb_vscroll.v and tb_edge.v replace the arbiter+vram
 * with an idealized one-cycle-ack slave, which is exactly the
 * difference that let them PASS while hardware failed.
 *
 * Snapshot method: source region copied to a separate array BEFORE the
 * blit; destination compared against that snapshot, never live memory.
 */
module tb_system_vscroll;
    reg clk=0,rst=1; always #5 clk=~clk;

    // blitter slave port (CPU register writes)
    reg wb_cyc=0,wb_stb=0,wb_we=0; reg [31:0] wb_adr=0,wb_dat=0;
    wire [31:0] wb_dat_o; wire wb_ack;

    // blitter m port -> arbiter m2
    wire m_cyc,m_stb,m_we; wire [3:0] m_sel; wire [31:0] m_adr,m_dat_o;
    wire [31:0] m_dat_i; wire m_ack;

    // s port unused (VRAM source)
    wire s_cyc,s_stb,s_we; wire [3:0] s_sel; wire [31:0] s_adr;
    wire [11:0] glyph_addr; wire busy;

    gpu_blit_wb dut(.clk(clk),.rst(rst),
      .wb_cyc_i(wb_cyc),.wb_stb_i(wb_stb),.wb_we_i(wb_we),.wb_sel_i(4'hF),
      .wb_adr_i(wb_adr),.wb_dat_i(wb_dat),.wb_ack_o(wb_ack),.wb_dat_o(wb_dat_o),
      .m_cyc_o(m_cyc),.m_stb_o(m_stb),.m_we_o(m_we),.m_sel_o(m_sel),
      .m_adr_o(m_adr),.m_dat_o(m_dat_o),.m_dat_i(m_dat_i),.m_ack_i(m_ack),
      .s_cyc_o(s_cyc),.s_stb_o(s_stb),.s_we_o(s_we),.s_sel_o(s_sel),
      .s_adr_o(s_adr),.s_dat_i(32'h0),.s_ack_i(1'b0),
      .glyph_addr_o(glyph_addr),.glyph_data_i(8'h00),.busy(busy));

    // real arbiter, CPU and raster masters idle
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

    // real vram, addressed exactly as sysctl.v does: byte adr [27:2]
    wire [15:0] gb_dat_unused_hi;
    wire [31:0] gb_dat; 
    vram_wb vram0(.wb_clk_i(clk),.wb_rst_i(rst),
      .wb_adr_i(a_adr[16:2]),.wb_dat_i(a_dat_o),.wb_dat_o(a_dat_i),
      .wb_we_i(a_we),.wb_sel_i(a_sel),.wb_stb_i(a_stb),.wb_ack_o(a_ack),
      .wb_cyc_i(a_cyc),.gb_adr_i(15'h0),.gb_dat_o(gb_dat));

    localparam VRAM_BASE=32'h20000000, WPL=20;
    reg [31:0] snap [0:9599];
    integer i,errors;

    task wb_write(input [31:0] a,input [31:0] d);
      begin @(posedge clk); wb_adr<=a; wb_dat<=d; wb_we<=1; wb_cyc<=1; wb_stb<=1;
        @(posedge clk); while(!wb_ack) @(posedge clk);
        wb_cyc<=0; wb_stb<=0; wb_we<=0; @(posedge clk); end endtask
    task wait_done; integer g; begin g=0;
      while(!busy && g<80) begin @(posedge clk); g=g+1; end
      while(busy) @(posedge clk); @(posedge clk); end endtask

    localparam R_CTRL=0,R_DX=2,R_DY=3,R_W=4,R_H=5,R_SA=12,R_SS=13,R_SH=14;
    localparam C_START=32'h1, C_CLIP=32'h4;

    /* exactly what z_fb_hw_blit_vram() does */
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

    /* snapshot-first check: dest row (dy+r) must equal the SNAPSHOT of
       source row (sy+r) inside [dx0,dx0+ww); everything else on screen
       must equal the snapshot of itself. Never compares against live
       memory. */
    integer px,r,bad,firstbad,outside;
    reg expb,gotb;
    integer sx0,sy0,dx0,dy0,ww,hh,rep;
    reg [31:0] patt;

    task seed;
      begin
        for(i=0;i<9600;i=i+1)
          vram0.vram[i] = {16'hC0DE ^ (i/WPL), 6'h15, (i%WPL)*32'd1, 4'h9} ^ ((i/WPL)<<20);
      end endtask

    task take_snap;
      begin for(i=0;i<9600;i=i+1) snap[i]=vram0.vram[i]; end endtask

    task check_copy(input [255:0] label);
      begin
        for(r=0;r<hh;r=r+1) begin
          bad=0; firstbad=-1;
          for(px=dx0; px<dx0+ww; px=px+1) begin
            expb = snap[(sy0+r)*WPL + ((px+sx0-dx0)>>5)][(px+sx0-dx0)%32];
            gotb = vram0.vram[(dy0+r)*WPL + (px>>5)][px%32];
            if(expb!==gotb) begin bad=bad+1; if(firstbad<0) firstbad=px; end
          end
          if(bad) begin
            errors=errors+1;
            if (errors<=8)
              $display("  FAIL %0s: row dy=%0d: %0d/%0d px wrong, first bad x=%0d (row word %0d)",
                label, dy0+r, bad, ww, firstbad, (firstbad>>5)-(dx0>>5));
          end
        end
        outside=0;
        for(i=0;i<9600;i=i+1)
          for(px=0;px<32;px=px+1)
            if (!((i/WPL)>=dy0 && (i/WPL)<dy0+hh &&
                  ((i%WPL)*32+px)>=dx0 && ((i%WPL)*32+px)<dx0+ww))
              if (snap[i][px]!==vram0.vram[i][px]) outside=outside+1;
        if(outside) begin
          $display("  FAIL %0s: %0d pixels OUTSIDE the rect modified", label, outside);
          errors=errors+1;
        end
      end endtask

    /* one scroll case: seed, snapshot, blit, check */
    task scroll_case(input integer sx,input integer sy,
                     input integer dx,input integer dy,
                     input integer w,input integer h,
                     input [255:0] label);
      begin
        sx0=sx; sy0=sy; dx0=dx; dy0=dy; ww=w; hh=h;
        seed; take_snap;
        blit_vram(sx0,sy0, dx0,dy0, ww,hh);
        check_copy(label);
      end endtask

    initial begin
      errors=0;
      repeat(8) @(posedge clk); rst=0; repeat(6) @(posedge clk);

      /* 1. the exact hardware case: x=46 y=53 w=303 h=225 dy=-9 */
      scroll_case(46,62, 46,53, 303,225, "hw case x=46 dy=-9");

      /* 2. alignment sweep, same-x vertical scrolls */
      scroll_case( 0,100,  0,90, 320,4, "aligned x=0");
      scroll_case(32,100, 32,90, 320,4, "aligned x=32");
      scroll_case(44,100, 44,90, 303,4, "x=44");
      scroll_case(12,100, 12,90, 200,4, "x=12");
      scroll_case(63,100, 63,90, 100,4, "x=63");
      scroll_case(31,100, 31,90,   1,4, "1px wide");

      /* 3. sx != dx, including the PRIME (sbit0<0) case */
      scroll_case( 2,100, 40,90, 120,3, "prime sx=2 dx=40");
      scroll_case(70,100, 40,90, 120,3, "sx>dx shift");
      scroll_case(40,100, 41,90, 120,3, "shift by 1");

      /* 4. repeated scrolls of the same rect, like a terminal --
            each iteration re-snapshots so overlap is checked honestly */
      sx0=46; sy0=62; dx0=46; dy0=53; ww=303; hh=100;
      seed;
      for (rep=0; rep<3; rep=rep+1) begin
        take_snap;
        blit_vram(sx0,sy0, dx0,dy0, ww,hh);
        check_copy("repeat scroll");
      end

      /* 5. fills through the real arbiter: the held-cyc rhythm the
            fix must not disturb. Unclipped, clipped, and XOR. */
      patt = 32'hDEADBEEF;
      wb_write(6, patt);
      seed; take_snap;
      wb_write(R_DX,64); wb_write(R_DY,20); wb_write(R_W,64); wb_write(R_H,2);
      wb_write(R_CTRL, C_START | 32'h2); /* unclipped fill */
      wait_done();
      bad=0;
      for(r=20;r<22;r=r+1) begin
        if (vram0.vram[r*WPL+2] !== 32'hDEADBEEF) bad=bad+1;
        if (vram0.vram[r*WPL+3] !== 32'hDEADBEEF) bad=bad+1;
        if (vram0.vram[r*WPL+1] !== snap[r*WPL+1]) bad=bad+1;
        if (vram0.vram[r*WPL+4] !== snap[r*WPL+4]) bad=bad+1;
      end
      if(bad) begin errors=errors+1; $display("  FAIL unclipped fill: %0d words", bad); end

      seed; take_snap;
      wb_write(R_DX,46); wb_write(R_DY,30); wb_write(R_W,100); wb_write(R_H,2);
      wb_write(R_CTRL, C_START | 32'h2 | C_CLIP); /* clipped fill */
      wait_done();
      bad=0;
      for(r=30;r<32;r=r+1)
        for(px=0;px<640;px=px+1) begin
          if (px>=46 && px<146) expb = patt[px%32];
          else expb = snap[r*WPL+(px>>5)][px%32];
          gotb = vram0.vram[r*WPL+(px>>5)][px%32];
          if (expb!==gotb) bad=bad+1;
        end
      if(bad) begin errors=errors+1; $display("  FAIL clipped fill: %0d px", bad); end

      seed; take_snap;
      wb_write(R_DX,46); wb_write(R_DY,40); wb_write(R_W,100); wb_write(R_H,2);
      wb_write(R_CTRL, C_START | 32'h2 | C_CLIP | 32'h40); /* XOR fill */
      wait_done();
      bad=0;
      for(r=40;r<42;r=r+1)
        for(px=0;px<640;px=px+1) begin
          if (px>=46 && px<146) expb = snap[r*WPL+(px>>5)][px%32] ^ patt[px%32];
          else expb = snap[r*WPL+(px>>5)][px%32];
          gotb = vram0.vram[r*WPL+(px>>5)][px%32];
          if (expb!==gotb) bad=bad+1;
        end
      if(bad) begin errors=errors+1; $display("  FAIL XOR fill: %0d px", bad); end

      if(errors==0) $display("RESULT: PASS -- all system-level cases correct");
      else $display("RESULT: FAIL (%0d)", errors);
      $finish;
    end
endmodule

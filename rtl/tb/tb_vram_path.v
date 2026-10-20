/*
 * tb_vram_path.v -- the CPU's path to VRAM, as rtl/sysctl.v wires it:
 * CPU -> MPU -> cache (wb_cache, bypass) -> wb_arbiter_main -> main bus,
 * and the VRAM bus behind wb_arbiter_vram, with a second master (a
 * "blitter") writing VRAM at the same time.
 *
 * Two wirings of the arbiter's CPU port, chosen by NEW:
 *   0  the CPU's own signals after the MPU (wbm_cpu_*_p) -- as it was
 *   1  the main bus (wbm_*) while the CPU holds it -- as it is now
 *
 * Checks, for both: every CPU store and load to VRAM gives the right
 * data (byte selects included), interleaved with main-memory accesses
 * and with the second master writing elsewhere in VRAM; a store the
 * MPU blocks never reaches VRAM; and how many cycles a CPU access to
 * VRAM takes, printed so the two wirings can be compared.
 *
 *   iverilog -g2005 -Ptb_vram_path.NEW=1 -o /tmp/tb_vp rtl/tb/tb_vram_path.v \
 *       rtl/mpu.v rtl/cache_id.v rtl/arbiter_main.v rtl/arbiter_vram.v \
 *       rtl/mem/vram.v && vvp -n /tmp/tb_vp
 *
 * Results (NEW=0 / NEW=1): both PASS; a CPU access to VRAM takes 7
 * cycles either way with nothing else on the VRAM bus, and on average
 * 9.6 / 9.0 with the second master busy -- the old wiring could take
 * the VRAM bus for the CPU on a stale address decode, holding off the
 * blitter for nothing. With MASK allowing nibble 2 the MPU check fails
 * (words 7 and 8 written), which is how the test knows it tests that.
 */

`timescale 1ns / 1ps

module tb_vram_path;

    parameter NEW = 1;
    parameter SEED = 1;
    parameter NOPS = 4000;

    reg clk, rst;
    initial clk = 0;
    always #5 clk = ~clk;

    // -- CPU side ---------------------------------------------------
    reg [31:0] cpu_adr, cpu_dat_o;
    reg cpu_we, cpu_stb, cpu_cyc, cpu_instr;
    reg [3:0] cpu_sel;
    wire [31:0] cpu_dat_i;
    wire cpu_ack;

    reg [31:0] mtu_base;
    // the MTU on the bus (picorv32): 0x8xxx_xxxx is the app's block
    wire [31:0] cpu_padr = (cpu_adr[31:28] == 4'h8 && mtu_base != 0) ?
        (mtu_base + {4'h0, cpu_adr[27:0]}) : cpu_adr;

    wire cpu_stb_p, cpu_cyc_p, cpu_ack_p;
    wire [31:0] cpu_dat_i_p;

    wb_mpu #(.MAIN_512(1), .XLATE_ON_BUS(1)) mpu_i (
        .wb_clk_i(clk), .wb_rst_i(rst),
        .c_adr_i(cpu_adr), .c_dat_i(cpu_dat_o), .c_we_i(cpu_we),
        .c_sel_i(cpu_sel), .c_stb_i(cpu_stb), .c_cyc_i(cpu_cyc),
        .c_instr_i(cpu_instr), .c_dat_o(cpu_dat_i), .c_ack_o(cpu_ack),
        .d_stb_o(cpu_stb_p), .d_cyc_o(cpu_cyc_p),
        .d_dat_i(cpu_dat_i_p), .d_ack_i(cpu_ack_p),
        .mtu_base_i(mtu_base), .irq_o()
    );

    // -- cache --------------------------------------------------------
    wire [31:0] wbc_adr, wbc_dat_o, wbc_dat_i;
    wire wbc_we, wbc_stb, wbc_cyc, wbc_ack;
    wire [3:0] wbc_sel;
    wire [2:0] wbc_cti;

    wb_cache #(.MAIN_512(1), .I_KB(8), .I_LINE_WORDS(4), .D_KB(4),
        .D_LINE_WORDS(4), .FAST_HIT(1), .WBUF_DEPTH(2), .BURST(0),
        .SNOOP(0), .CFG_BASE(32'h7000_0100)) cache_i (
        .wb_clk_i(clk), .wb_rst_i(rst),
        .c_adr_i(cpu_padr), .c_dat_i(cpu_dat_o), .c_dat_o(cpu_dat_i_p),
        .c_we_i(cpu_we), .c_sel_i(cpu_sel), .c_stb_i(cpu_stb_p),
        .c_cyc_i(cpu_cyc_p), .c_instr_i(cpu_instr), .c_ack_o(cpu_ack_p),
        .m_adr_o(wbc_adr), .m_dat_o(wbc_dat_o), .m_dat_i(wbc_dat_i),
        .m_we_o(wbc_we), .m_sel_o(wbc_sel), .m_stb_o(wbc_stb),
        .m_cyc_o(wbc_cyc), .m_cti_o(wbc_cti), .m_bte_o(),
        .m_ack_i(wbc_ack), .snoop_stb_i(1'b0), .snoop_adr_i(32'h0),
        .c_cfg_hit()
    );

    // -- main bus -----------------------------------------------------
    wire [31:0] wbm_adr, wbm_dat_o;
    wire [31:0] wbm_dat_i;
    wire wbm_we, wbm_stb, wbm_cyc, wbm_ack;
    wire [3:0] wbm_sel;
    wire [1:0] marb_master;

    wb_arbiter_main marb0_i (
        .clk(clk), .rst(rst),
        .m0_adr_i(wbc_adr), .m0_dat_i(wbc_dat_o), .m0_dat_o(wbc_dat_i),
        .m0_we_i(wbc_we), .m0_sel_i(wbc_sel), .m0_stb_i(wbc_stb),
        .m0_cyc_i(wbc_cyc), .m0_ack_o(wbc_ack),
        .m1_adr_i(32'h0), .m1_dat_i(32'h0), .m1_dat_o(), .m1_we_i(1'b0),
        .m1_sel_i(4'h0), .m1_stb_i(1'b0), .m1_cyc_i(1'b0), .m1_ack_o(),
        .m2_adr_i(32'h0), .m2_dat_i(32'h0), .m2_dat_o(), .m2_we_i(1'b0),
        .m2_sel_i(4'h0), .m2_stb_i(1'b0), .m2_cyc_i(1'b0), .m2_ack_o(),
        .s_adr_o(wbm_adr), .s_dat_o(wbm_dat_o), .s_dat_i(wbm_dat_i),
        .s_we_o(wbm_we), .s_sel_o(wbm_sel), .s_stb_o(wbm_stb),
        .s_cyc_o(wbm_cyc), .s_ack_i(wbm_ack), .master(marb_master)
    );

    wire cs_vram = ((wbm_adr & 32'hf000_0000) == 32'h2000_0000);
    wire cs_main = ((wbm_adr & 32'he000_0000) == 32'h4000_0000);

    // main memory: a few cycles' latency, contents a function of the address
    reg main_ack;
    reg [31:0] main_dat;
    reg [1:0] main_wait;
    always @(posedge clk) begin
        main_ack <= 1'b0;
        if (rst) main_wait <= 0;
        else if (cs_main && wbm_cyc && wbm_stb && !main_ack) begin
            if (main_wait == 2) begin
                main_ack <= 1'b1;
                main_dat <= wbm_adr ^ 32'h5A5A_A5A5;
                main_wait <= 0;
            end else main_wait <= main_wait + 1;
        end
    end

    // -- the VRAM bus ---------------------------------------------------
    wire [31:0] arb0_dat_i;
    wire arb0_ack;
    wire arb0_cyc = NEW ? (cs_vram && wbm_cyc && (marb_master == 2'd0))
                        : (cs_vram && cpu_cyc_p);

    assign wbm_ack = (cs_main & main_ack) | (cs_vram & arb0_ack);
    assign wbm_dat_i = ({32{cs_main}} & main_dat) | ({32{cs_vram}} & arb0_dat_i);

    // second master: a "blitter" writing the top half of VRAM
    reg [31:0] g_adr, g_dat;
    reg g_stb, g_cyc;
    wire g_ack;

    wire [31:0] v_adr, v_dat_o, v_dat_i;
    wire v_we, v_stb, v_cyc, v_ack;
    wire [3:0] v_sel;

    wb_arbiter_vram varb0_i (
        .clk(clk), .rst(rst),
        .m0_adr_i(NEW ? wbm_adr : cpu_adr),
        .m0_dat_i(NEW ? wbm_dat_o : cpu_dat_o),
        .m0_dat_o(arb0_dat_i),
        .m0_we_i(NEW ? wbm_we : cpu_we),
        .m0_sel_i(NEW ? wbm_sel : cpu_sel),
        .m0_stb_i(NEW ? wbm_stb : cpu_stb_p),
        .m0_cyc_i(arb0_cyc),
        .m0_ack_o(arb0_ack),
        .m1_adr_i(32'h0), .m1_dat_i(32'h0), .m1_dat_o(), .m1_we_i(1'b0),
        .m1_sel_i(4'h0), .m1_stb_i(1'b0), .m1_cyc_i(1'b0), .m1_ack_o(),
        .m2_adr_i(g_adr), .m2_dat_i(g_dat), .m2_dat_o(), .m2_we_i(1'b1),
        .m2_sel_i(4'hF), .m2_stb_i(g_stb), .m2_cyc_i(g_cyc), .m2_ack_o(g_ack),
        .s_adr_o(v_adr), .s_dat_o(v_dat_o), .s_dat_i(v_dat_i),
        .s_we_o(v_we), .s_sel_o(v_sel), .s_stb_o(v_stb), .s_cyc_o(v_cyc),
        .s_ack_i(v_ack), .master()
    );

    wire [27:0] v_adr_sel = v_adr - 32'h2000_0000;

    vram_wb vram_i (
        .wb_clk_i(clk), .wb_rst_i(rst),
        .wb_adr_i(v_adr_sel[16:2]), .wb_dat_i(v_dat_o), .wb_dat_o(v_dat_i),
        .wb_we_i(v_we), .wb_sel_i(v_sel), .wb_stb_i(v_stb),
        .wb_ack_o(v_ack), .wb_cyc_i(v_cyc),
        .gb_adr_i(15'd0), .gb_dat_o()
    );

    // -- reference ------------------------------------------------------
    localparam WORDS = 9600;
    reg [31:0] model [0:WORDS-1];
    integer i, seed, errors, ops;
    integer cyc_total, cyc_n, cyc_min, cyc_max;
    reg [31:0] got;
    integer t;

    // the blitter: writes words 4800.. at random, all the time, and
    // records them in the model
    integer g_word;
    reg g_on;
    reg [0:WORDS-1] g_done;          // words the blitter has finished writing
    always @(posedge clk) begin
        if (rst) begin
            g_stb <= 0; g_cyc <= 0;
        end else if (g_cyc && g_ack) begin
            model[g_word] = g_dat;
            g_done[g_word] = 1'b1;
            g_stb <= 0; g_cyc <= 0;
        end else if (g_on && !g_cyc && !g_ack && ($unsigned($random(seed)) % 4 == 0)) begin
            g_word = 4800 + ($unsigned($random(seed)) % 4800);
            g_adr <= 32'h2000_0000 + g_word * 4;
            g_dat <= $random(seed);
            g_stb <= 1; g_cyc <= 1;
        end
    end

    task bus;
        input [31:0] a;
        input w;
        input f;
        input [31:0] d;
        input [3:0] s;
    begin
        @(posedge clk);
        cpu_adr <= a; cpu_we <= w; cpu_instr <= f; cpu_dat_o <= d;
        cpu_sel <= s; cpu_stb <= 1; cpu_cyc <= 1;
        t = 1;
        @(posedge clk);
        while (!cpu_ack) begin
            t = t + 1;
            if (t > 200) begin $display("TIMEOUT %h", a); $finish; end
            @(posedge clk);
        end
        got = cpu_dat_i;
        cpu_stb <= 0; cpu_cyc <= 0; cpu_we <= 0; cpu_instr <= 0;
    end
    endtask

    task vram_store;
        input integer word;
        input [31:0] d;
        input [3:0] s;
        reg [31:0] m;
    begin
        bus(32'h2000_0000 + word * 4, 1, 0, d, s);
        m = model[word];
        if (s[0]) m[7:0] = d[7:0];
        if (s[1]) m[15:8] = d[15:8];
        if (s[2]) m[23:16] = d[23:16];
        if (s[3]) m[31:24] = d[31:24];
        model[word] = m;
        cyc_total = cyc_total + t; cyc_n = cyc_n + 1;
        if (t < cyc_min) cyc_min = t;
        if (t > cyc_max) cyc_max = t;
    end
    endtask

    task vram_load;
        input integer word;
        reg mine;
    begin
        bus(32'h2000_0000 + word * 4, 0, 0, 0, 4'h0);
        cyc_total = cyc_total + t; cyc_n = cyc_n + 1;
        if (t < cyc_min) cyc_min = t;
        if (t > cyc_max) cyc_max = t;
        // the blitter's half: only words it has finished, and not the
        // one it is writing now
        mine = (word < 4800) || (g_done[word] && !(g_cyc && g_word == word));
        if (mine && got !== model[word]) begin
            $display("ERROR: VRAM word %0d read %h expected %h", word, got, model[word]);
            errors = errors + 1;
        end
    end
    endtask

    integer w;
    reg [3:0] s;

    initial begin
        seed = SEED; errors = 0;
        cyc_total = 0; cyc_n = 0; cyc_min = 999; cyc_max = 0;
        cpu_adr = 0; cpu_dat_o = 0; cpu_we = 0; cpu_stb = 0; cpu_cyc = 0;
        cpu_instr = 0; cpu_sel = 0; mtu_base = 0;
        for (i = 0; i < WORDS; i = i + 1) model[i] = 32'h0;
        g_done = 0; g_on = 0;
        rst = 1; repeat (4) @(posedge clk); rst = 0;
        repeat (1200) @(posedge clk);       // the caches invalidate themselves after reset

        // VRAM starts as whatever the simulator gives a reg array (x):
        // write the CPU's half first so the model knows it.
        for (i = 0; i < 4800; i = i + 1) begin
            bus(32'h2000_0000 + i * 4, 1, 0, i * 32'h01010101, 4'hF);
            model[i] = i * 32'h01010101;
        end

        // -- latency alone: back-to-back stores and loads, nothing else on
        //    the VRAM bus
        for (i = 0; i < 64; i = i + 1) vram_store(200 + i, 32'hA000_0000 + i, 4'hF);
        for (i = 0; i < 64; i = i + 1) vram_load(200 + i);
        $display("NEW=%0d: CPU access to VRAM alone: %0d-%0d cycles, average %0d.%02d",
            NEW, cyc_min, cyc_max, cyc_total / cyc_n, (cyc_total * 100 / cyc_n) % 100);
        cyc_total = 0; cyc_n = 0; cyc_min = 999; cyc_max = 0;
        g_on = 1;

        // -- random CPU traffic: VRAM stores (any byte selects), VRAM
        //    loads, main memory loads and stores, fetches
        for (ops = 0; ops < NOPS; ops = ops + 1) begin
            w = $unsigned($random(seed)) % 4800;
            case ($unsigned($random(seed)) % 6)
                0, 1: begin
                    s = $random(seed);
                    if (s == 0) s = 4'hF;
                    vram_store(w, $random(seed), s);
                end
                2, 3: vram_load(w);
                4: bus(32'h4010_0000 + w * 4, $random(seed), 0, $random(seed), 4'hF);
                5: vram_load($unsigned($random(seed)) % 4800 + 4800);  // the blitter's half
            endcase
        end

        // -- back to back: stores then loads, nothing between
        for (i = 0; i < 64; i = i + 1) vram_store(100 + i, 32'hC0DE_0000 + i, 4'hF);
        for (i = 0; i < 64; i = i + 1) vram_load(100 + i);

        $display("NEW=%0d: with the blitter busy: %0d-%0d cycles, average %0d.%02d (%0d accesses)",
            NEW, cyc_min, cyc_max, cyc_total / cyc_n, (cyc_total * 100 / cyc_n) % 100, cyc_n);

        // -- the MPU blocks an app's store to VRAM ---------------------
        g_on = 0;
        repeat (10) @(posedge clk);
        // (as tb_mpu.v: program it from kernel code, then run app code)
        mtu_base = 32'h4008_0000;
        bus(32'h4000_0400, 0, 1, 0, 4'h0);                 // kernel fetch
        bus(32'h9000_0100 + 1*4, 1, 0, 32'h4001_0000, 4'hF); // KTEXT
        bus(32'h9000_0100 + 2*4, 1, 0, 32'h4000_0100, 4'hF); // GATE
        bus(32'h9000_0100 + 3*4, 1, 0, 32'h0000_4000, 4'hF); // SIZE
        bus(32'h9000_0100 + 4*4, 1, 0, 32'h0000_FFFB, 4'hF); // MASK: nibble 2 (VRAM) off
        bus(32'h9000_0100 + 0*4, 1, 0, 32'h3, 4'hF);         // enable, enforce
        bus(32'h8000_0000, 0, 1, 0, 4'h0);                 // into the app
        bus(32'h2000_0000 + 7 * 4, 1, 0, 32'hDEAD_BEEF, 4'hF);
        bus(32'h2000_0000 + 8 * 4, 1, 0, 32'hDEAD_BEEF, 4'h3);
        bus(32'h4000_0400, 0, 1, 0, 4'h0);                 // back to the kernel? (gate rules aside)
        // read back with the MPU off (from kernel code, through the gate)
        mpu_i.en = 1'b0;
        vram_load(7);
        vram_load(8);

        if (errors == 0) $display("tb_vram_path NEW=%0d: PASS", NEW);
        else $display("tb_vram_path NEW=%0d: FAIL (%0d errors)", NEW, errors);
        $finish;
    end

endmodule

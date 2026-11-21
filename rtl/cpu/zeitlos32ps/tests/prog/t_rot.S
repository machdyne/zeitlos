/*
 * zeitlos32ps -- Zbb rotates (ENABLE_ROT): ror, rol, rori, and the
 * shifts sharing their funnel shifter. Check ids 1000-1999.
 */

#include "test.h"

	.option arch, +zbb
	TEST_ENTRY

	li	a0, 0x12345678
	li	a1, 0
	ror	a2, a0, a1
	CHECK_EQ 1000, a2, 0x12345678
	rol	a2, a0, a1
	CHECK_EQ 1001, a2, 0x12345678
	sll	a2, a0, a1
	CHECK_EQ 1002, a2, 0x12345678
	srl	a2, a0, a1
	CHECK_EQ 1003, a2, 0x12345678
	sra	a2, a0, a1
	CHECK_EQ 1004, a2, 0x12345678
	rori	a2, a0, 0
	CHECK_EQ 1005, a2, 0x12345678
	slli	a2, a0, 0
	CHECK_EQ 1006, a2, 0x12345678
	srai	a2, a0, 0
	CHECK_EQ 1007, a2, 0x12345678
	li	a0, 0x12345678
	li	a1, 1
	ror	a2, a0, a1
	CHECK_EQ 1008, a2, 0x091a2b3c
	rol	a2, a0, a1
	CHECK_EQ 1009, a2, 0x2468acf0
	sll	a2, a0, a1
	CHECK_EQ 1010, a2, 0x2468acf0
	srl	a2, a0, a1
	CHECK_EQ 1011, a2, 0x091a2b3c
	sra	a2, a0, a1
	CHECK_EQ 1012, a2, 0x091a2b3c
	rori	a2, a0, 1
	CHECK_EQ 1013, a2, 0x091a2b3c
	slli	a2, a0, 1
	CHECK_EQ 1014, a2, 0x2468acf0
	srai	a2, a0, 1
	CHECK_EQ 1015, a2, 0x091a2b3c
	li	a0, 0x12345678
	li	a1, 4
	ror	a2, a0, a1
	CHECK_EQ 1016, a2, 0x81234567
	rol	a2, a0, a1
	CHECK_EQ 1017, a2, 0x23456781
	sll	a2, a0, a1
	CHECK_EQ 1018, a2, 0x23456780
	srl	a2, a0, a1
	CHECK_EQ 1019, a2, 0x01234567
	sra	a2, a0, a1
	CHECK_EQ 1020, a2, 0x01234567
	rori	a2, a0, 4
	CHECK_EQ 1021, a2, 0x81234567
	slli	a2, a0, 4
	CHECK_EQ 1022, a2, 0x23456780
	srai	a2, a0, 4
	CHECK_EQ 1023, a2, 0x01234567
	li	a0, 0x12345678
	li	a1, 8
	ror	a2, a0, a1
	CHECK_EQ 1024, a2, 0x78123456
	rol	a2, a0, a1
	CHECK_EQ 1025, a2, 0x34567812
	sll	a2, a0, a1
	CHECK_EQ 1026, a2, 0x34567800
	srl	a2, a0, a1
	CHECK_EQ 1027, a2, 0x00123456
	sra	a2, a0, a1
	CHECK_EQ 1028, a2, 0x00123456
	rori	a2, a0, 8
	CHECK_EQ 1029, a2, 0x78123456
	slli	a2, a0, 8
	CHECK_EQ 1030, a2, 0x34567800
	srai	a2, a0, 8
	CHECK_EQ 1031, a2, 0x00123456
	li	a0, 0x12345678
	li	a1, 13
	ror	a2, a0, a1
	CHECK_EQ 1032, a2, 0xb3c091a2
	rol	a2, a0, a1
	CHECK_EQ 1033, a2, 0x8acf0246
	sll	a2, a0, a1
	CHECK_EQ 1034, a2, 0x8acf0000
	srl	a2, a0, a1
	CHECK_EQ 1035, a2, 0x000091a2
	sra	a2, a0, a1
	CHECK_EQ 1036, a2, 0x000091a2
	rori	a2, a0, 13
	CHECK_EQ 1037, a2, 0xb3c091a2
	slli	a2, a0, 13
	CHECK_EQ 1038, a2, 0x8acf0000
	srai	a2, a0, 13
	CHECK_EQ 1039, a2, 0x000091a2
	li	a0, 0x12345678
	li	a1, 16
	ror	a2, a0, a1
	CHECK_EQ 1040, a2, 0x56781234
	rol	a2, a0, a1
	CHECK_EQ 1041, a2, 0x56781234
	sll	a2, a0, a1
	CHECK_EQ 1042, a2, 0x56780000
	srl	a2, a0, a1
	CHECK_EQ 1043, a2, 0x00001234
	sra	a2, a0, a1
	CHECK_EQ 1044, a2, 0x00001234
	rori	a2, a0, 16
	CHECK_EQ 1045, a2, 0x56781234
	slli	a2, a0, 16
	CHECK_EQ 1046, a2, 0x56780000
	srai	a2, a0, 16
	CHECK_EQ 1047, a2, 0x00001234
	li	a0, 0x12345678
	li	a1, 31
	ror	a2, a0, a1
	CHECK_EQ 1048, a2, 0x2468acf0
	rol	a2, a0, a1
	CHECK_EQ 1049, a2, 0x091a2b3c
	sll	a2, a0, a1
	CHECK_EQ 1050, a2, 0x00000000
	srl	a2, a0, a1
	CHECK_EQ 1051, a2, 0x00000000
	sra	a2, a0, a1
	CHECK_EQ 1052, a2, 0x00000000
	rori	a2, a0, 31
	CHECK_EQ 1053, a2, 0x2468acf0
	slli	a2, a0, 31
	CHECK_EQ 1054, a2, 0x00000000
	srai	a2, a0, 31
	CHECK_EQ 1055, a2, 0x00000000
	li	a0, 0x12345678
	li	a1, 32
	ror	a2, a0, a1
	CHECK_EQ 1056, a2, 0x12345678
	rol	a2, a0, a1
	CHECK_EQ 1057, a2, 0x12345678
	sll	a2, a0, a1
	CHECK_EQ 1058, a2, 0x12345678
	srl	a2, a0, a1
	CHECK_EQ 1059, a2, 0x12345678
	sra	a2, a0, a1
	CHECK_EQ 1060, a2, 0x12345678
	li	a0, 0x12345678
	li	a1, 33
	ror	a2, a0, a1
	CHECK_EQ 1064, a2, 0x091a2b3c
	rol	a2, a0, a1
	CHECK_EQ 1065, a2, 0x2468acf0
	sll	a2, a0, a1
	CHECK_EQ 1066, a2, 0x2468acf0
	srl	a2, a0, a1
	CHECK_EQ 1067, a2, 0x091a2b3c
	sra	a2, a0, a1
	CHECK_EQ 1068, a2, 0x091a2b3c
	li	a0, 0x12345678
	li	a1, 63
	ror	a2, a0, a1
	CHECK_EQ 1072, a2, 0x2468acf0
	rol	a2, a0, a1
	CHECK_EQ 1073, a2, 0x091a2b3c
	sll	a2, a0, a1
	CHECK_EQ 1074, a2, 0x00000000
	srl	a2, a0, a1
	CHECK_EQ 1075, a2, 0x00000000
	sra	a2, a0, a1
	CHECK_EQ 1076, a2, 0x00000000
	li	a0, 0x80000001
	li	a1, 0
	ror	a2, a0, a1
	CHECK_EQ 1080, a2, 0x80000001
	rol	a2, a0, a1
	CHECK_EQ 1081, a2, 0x80000001
	sll	a2, a0, a1
	CHECK_EQ 1082, a2, 0x80000001
	srl	a2, a0, a1
	CHECK_EQ 1083, a2, 0x80000001
	sra	a2, a0, a1
	CHECK_EQ 1084, a2, 0x80000001
	rori	a2, a0, 0
	CHECK_EQ 1085, a2, 0x80000001
	slli	a2, a0, 0
	CHECK_EQ 1086, a2, 0x80000001
	srai	a2, a0, 0
	CHECK_EQ 1087, a2, 0x80000001
	li	a0, 0x80000001
	li	a1, 1
	ror	a2, a0, a1
	CHECK_EQ 1088, a2, 0xc0000000
	rol	a2, a0, a1
	CHECK_EQ 1089, a2, 0x00000003
	sll	a2, a0, a1
	CHECK_EQ 1090, a2, 0x00000002
	srl	a2, a0, a1
	CHECK_EQ 1091, a2, 0x40000000
	sra	a2, a0, a1
	CHECK_EQ 1092, a2, 0xc0000000
	rori	a2, a0, 1
	CHECK_EQ 1093, a2, 0xc0000000
	slli	a2, a0, 1
	CHECK_EQ 1094, a2, 0x00000002
	srai	a2, a0, 1
	CHECK_EQ 1095, a2, 0xc0000000
	li	a0, 0x80000001
	li	a1, 4
	ror	a2, a0, a1
	CHECK_EQ 1096, a2, 0x18000000
	rol	a2, a0, a1
	CHECK_EQ 1097, a2, 0x00000018
	sll	a2, a0, a1
	CHECK_EQ 1098, a2, 0x00000010
	srl	a2, a0, a1
	CHECK_EQ 1099, a2, 0x08000000
	sra	a2, a0, a1
	CHECK_EQ 1100, a2, 0xf8000000
	rori	a2, a0, 4
	CHECK_EQ 1101, a2, 0x18000000
	slli	a2, a0, 4
	CHECK_EQ 1102, a2, 0x00000010
	srai	a2, a0, 4
	CHECK_EQ 1103, a2, 0xf8000000
	li	a0, 0x80000001
	li	a1, 8
	ror	a2, a0, a1
	CHECK_EQ 1104, a2, 0x01800000
	rol	a2, a0, a1
	CHECK_EQ 1105, a2, 0x00000180
	sll	a2, a0, a1
	CHECK_EQ 1106, a2, 0x00000100
	srl	a2, a0, a1
	CHECK_EQ 1107, a2, 0x00800000
	sra	a2, a0, a1
	CHECK_EQ 1108, a2, 0xff800000
	rori	a2, a0, 8
	CHECK_EQ 1109, a2, 0x01800000
	slli	a2, a0, 8
	CHECK_EQ 1110, a2, 0x00000100
	srai	a2, a0, 8
	CHECK_EQ 1111, a2, 0xff800000
	li	a0, 0x80000001
	li	a1, 13
	ror	a2, a0, a1
	CHECK_EQ 1112, a2, 0x000c0000
	rol	a2, a0, a1
	CHECK_EQ 1113, a2, 0x00003000
	sll	a2, a0, a1
	CHECK_EQ 1114, a2, 0x00002000
	srl	a2, a0, a1
	CHECK_EQ 1115, a2, 0x00040000
	sra	a2, a0, a1
	CHECK_EQ 1116, a2, 0xfffc0000
	rori	a2, a0, 13
	CHECK_EQ 1117, a2, 0x000c0000
	slli	a2, a0, 13
	CHECK_EQ 1118, a2, 0x00002000
	srai	a2, a0, 13
	CHECK_EQ 1119, a2, 0xfffc0000
	li	a0, 0x80000001
	li	a1, 16
	ror	a2, a0, a1
	CHECK_EQ 1120, a2, 0x00018000
	rol	a2, a0, a1
	CHECK_EQ 1121, a2, 0x00018000
	sll	a2, a0, a1
	CHECK_EQ 1122, a2, 0x00010000
	srl	a2, a0, a1
	CHECK_EQ 1123, a2, 0x00008000
	sra	a2, a0, a1
	CHECK_EQ 1124, a2, 0xffff8000
	rori	a2, a0, 16
	CHECK_EQ 1125, a2, 0x00018000
	slli	a2, a0, 16
	CHECK_EQ 1126, a2, 0x00010000
	srai	a2, a0, 16
	CHECK_EQ 1127, a2, 0xffff8000
	li	a0, 0x80000001
	li	a1, 31
	ror	a2, a0, a1
	CHECK_EQ 1128, a2, 0x00000003
	rol	a2, a0, a1
	CHECK_EQ 1129, a2, 0xc0000000
	sll	a2, a0, a1
	CHECK_EQ 1130, a2, 0x80000000
	srl	a2, a0, a1
	CHECK_EQ 1131, a2, 0x00000001
	sra	a2, a0, a1
	CHECK_EQ 1132, a2, 0xffffffff
	rori	a2, a0, 31
	CHECK_EQ 1133, a2, 0x00000003
	slli	a2, a0, 31
	CHECK_EQ 1134, a2, 0x80000000
	srai	a2, a0, 31
	CHECK_EQ 1135, a2, 0xffffffff
	li	a0, 0x80000001
	li	a1, 32
	ror	a2, a0, a1
	CHECK_EQ 1136, a2, 0x80000001
	rol	a2, a0, a1
	CHECK_EQ 1137, a2, 0x80000001
	sll	a2, a0, a1
	CHECK_EQ 1138, a2, 0x80000001
	srl	a2, a0, a1
	CHECK_EQ 1139, a2, 0x80000001
	sra	a2, a0, a1
	CHECK_EQ 1140, a2, 0x80000001
	li	a0, 0x80000001
	li	a1, 33
	ror	a2, a0, a1
	CHECK_EQ 1144, a2, 0xc0000000
	rol	a2, a0, a1
	CHECK_EQ 1145, a2, 0x00000003
	sll	a2, a0, a1
	CHECK_EQ 1146, a2, 0x00000002
	srl	a2, a0, a1
	CHECK_EQ 1147, a2, 0x40000000
	sra	a2, a0, a1
	CHECK_EQ 1148, a2, 0xc0000000
	li	a0, 0x80000001
	li	a1, 63
	ror	a2, a0, a1
	CHECK_EQ 1152, a2, 0x00000003
	rol	a2, a0, a1
	CHECK_EQ 1153, a2, 0xc0000000
	sll	a2, a0, a1
	CHECK_EQ 1154, a2, 0x80000000
	srl	a2, a0, a1
	CHECK_EQ 1155, a2, 0x00000001
	sra	a2, a0, a1
	CHECK_EQ 1156, a2, 0xffffffff
	li	a0, 0xdeadbeef
	li	a1, 0
	ror	a2, a0, a1
	CHECK_EQ 1160, a2, 0xdeadbeef
	rol	a2, a0, a1
	CHECK_EQ 1161, a2, 0xdeadbeef
	sll	a2, a0, a1
	CHECK_EQ 1162, a2, 0xdeadbeef
	srl	a2, a0, a1
	CHECK_EQ 1163, a2, 0xdeadbeef
	sra	a2, a0, a1
	CHECK_EQ 1164, a2, 0xdeadbeef
	rori	a2, a0, 0
	CHECK_EQ 1165, a2, 0xdeadbeef
	slli	a2, a0, 0
	CHECK_EQ 1166, a2, 0xdeadbeef
	srai	a2, a0, 0
	CHECK_EQ 1167, a2, 0xdeadbeef
	li	a0, 0xdeadbeef
	li	a1, 1
	ror	a2, a0, a1
	CHECK_EQ 1168, a2, 0xef56df77
	rol	a2, a0, a1
	CHECK_EQ 1169, a2, 0xbd5b7ddf
	sll	a2, a0, a1
	CHECK_EQ 1170, a2, 0xbd5b7dde
	srl	a2, a0, a1
	CHECK_EQ 1171, a2, 0x6f56df77
	sra	a2, a0, a1
	CHECK_EQ 1172, a2, 0xef56df77
	rori	a2, a0, 1
	CHECK_EQ 1173, a2, 0xef56df77
	slli	a2, a0, 1
	CHECK_EQ 1174, a2, 0xbd5b7dde
	srai	a2, a0, 1
	CHECK_EQ 1175, a2, 0xef56df77
	li	a0, 0xdeadbeef
	li	a1, 4
	ror	a2, a0, a1
	CHECK_EQ 1176, a2, 0xfdeadbee
	rol	a2, a0, a1
	CHECK_EQ 1177, a2, 0xeadbeefd
	sll	a2, a0, a1
	CHECK_EQ 1178, a2, 0xeadbeef0
	srl	a2, a0, a1
	CHECK_EQ 1179, a2, 0x0deadbee
	sra	a2, a0, a1
	CHECK_EQ 1180, a2, 0xfdeadbee
	rori	a2, a0, 4
	CHECK_EQ 1181, a2, 0xfdeadbee
	slli	a2, a0, 4
	CHECK_EQ 1182, a2, 0xeadbeef0
	srai	a2, a0, 4
	CHECK_EQ 1183, a2, 0xfdeadbee
	li	a0, 0xdeadbeef
	li	a1, 8
	ror	a2, a0, a1
	CHECK_EQ 1184, a2, 0xefdeadbe
	rol	a2, a0, a1
	CHECK_EQ 1185, a2, 0xadbeefde
	sll	a2, a0, a1
	CHECK_EQ 1186, a2, 0xadbeef00
	srl	a2, a0, a1
	CHECK_EQ 1187, a2, 0x00deadbe
	sra	a2, a0, a1
	CHECK_EQ 1188, a2, 0xffdeadbe
	rori	a2, a0, 8
	CHECK_EQ 1189, a2, 0xefdeadbe
	slli	a2, a0, 8
	CHECK_EQ 1190, a2, 0xadbeef00
	srai	a2, a0, 8
	CHECK_EQ 1191, a2, 0xffdeadbe
	li	a0, 0xdeadbeef
	li	a1, 13
	ror	a2, a0, a1
	CHECK_EQ 1192, a2, 0xf77ef56d
	rol	a2, a0, a1
	CHECK_EQ 1193, a2, 0xb7ddfbd5
	sll	a2, a0, a1
	CHECK_EQ 1194, a2, 0xb7dde000
	srl	a2, a0, a1
	CHECK_EQ 1195, a2, 0x0006f56d
	sra	a2, a0, a1
	CHECK_EQ 1196, a2, 0xfffef56d
	rori	a2, a0, 13
	CHECK_EQ 1197, a2, 0xf77ef56d
	slli	a2, a0, 13
	CHECK_EQ 1198, a2, 0xb7dde000
	srai	a2, a0, 13
	CHECK_EQ 1199, a2, 0xfffef56d
	li	a0, 0xdeadbeef
	li	a1, 16
	ror	a2, a0, a1
	CHECK_EQ 1200, a2, 0xbeefdead
	rol	a2, a0, a1
	CHECK_EQ 1201, a2, 0xbeefdead
	sll	a2, a0, a1
	CHECK_EQ 1202, a2, 0xbeef0000
	srl	a2, a0, a1
	CHECK_EQ 1203, a2, 0x0000dead
	sra	a2, a0, a1
	CHECK_EQ 1204, a2, 0xffffdead
	rori	a2, a0, 16
	CHECK_EQ 1205, a2, 0xbeefdead
	slli	a2, a0, 16
	CHECK_EQ 1206, a2, 0xbeef0000
	srai	a2, a0, 16
	CHECK_EQ 1207, a2, 0xffffdead
	li	a0, 0xdeadbeef
	li	a1, 31
	ror	a2, a0, a1
	CHECK_EQ 1208, a2, 0xbd5b7ddf
	rol	a2, a0, a1
	CHECK_EQ 1209, a2, 0xef56df77
	sll	a2, a0, a1
	CHECK_EQ 1210, a2, 0x80000000
	srl	a2, a0, a1
	CHECK_EQ 1211, a2, 0x00000001
	sra	a2, a0, a1
	CHECK_EQ 1212, a2, 0xffffffff
	rori	a2, a0, 31
	CHECK_EQ 1213, a2, 0xbd5b7ddf
	slli	a2, a0, 31
	CHECK_EQ 1214, a2, 0x80000000
	srai	a2, a0, 31
	CHECK_EQ 1215, a2, 0xffffffff
	li	a0, 0xdeadbeef
	li	a1, 32
	ror	a2, a0, a1
	CHECK_EQ 1216, a2, 0xdeadbeef
	rol	a2, a0, a1
	CHECK_EQ 1217, a2, 0xdeadbeef
	sll	a2, a0, a1
	CHECK_EQ 1218, a2, 0xdeadbeef
	srl	a2, a0, a1
	CHECK_EQ 1219, a2, 0xdeadbeef
	sra	a2, a0, a1
	CHECK_EQ 1220, a2, 0xdeadbeef
	li	a0, 0xdeadbeef
	li	a1, 33
	ror	a2, a0, a1
	CHECK_EQ 1224, a2, 0xef56df77
	rol	a2, a0, a1
	CHECK_EQ 1225, a2, 0xbd5b7ddf
	sll	a2, a0, a1
	CHECK_EQ 1226, a2, 0xbd5b7dde
	srl	a2, a0, a1
	CHECK_EQ 1227, a2, 0x6f56df77
	sra	a2, a0, a1
	CHECK_EQ 1228, a2, 0xef56df77
	li	a0, 0xdeadbeef
	li	a1, 63
	ror	a2, a0, a1
	CHECK_EQ 1232, a2, 0xbd5b7ddf
	rol	a2, a0, a1
	CHECK_EQ 1233, a2, 0xef56df77
	sll	a2, a0, a1
	CHECK_EQ 1234, a2, 0x80000000
	srl	a2, a0, a1
	CHECK_EQ 1235, a2, 0x00000001
	sra	a2, a0, a1
	CHECK_EQ 1236, a2, 0xffffffff
	li	a0, 0x00000001
	li	a1, 0
	ror	a2, a0, a1
	CHECK_EQ 1240, a2, 0x00000001
	rol	a2, a0, a1
	CHECK_EQ 1241, a2, 0x00000001
	sll	a2, a0, a1
	CHECK_EQ 1242, a2, 0x00000001
	srl	a2, a0, a1
	CHECK_EQ 1243, a2, 0x00000001
	sra	a2, a0, a1
	CHECK_EQ 1244, a2, 0x00000001
	rori	a2, a0, 0
	CHECK_EQ 1245, a2, 0x00000001
	slli	a2, a0, 0
	CHECK_EQ 1246, a2, 0x00000001
	srai	a2, a0, 0
	CHECK_EQ 1247, a2, 0x00000001
	li	a0, 0x00000001
	li	a1, 1
	ror	a2, a0, a1
	CHECK_EQ 1248, a2, 0x80000000
	rol	a2, a0, a1
	CHECK_EQ 1249, a2, 0x00000002
	sll	a2, a0, a1
	CHECK_EQ 1250, a2, 0x00000002
	srl	a2, a0, a1
	CHECK_EQ 1251, a2, 0x00000000
	sra	a2, a0, a1
	CHECK_EQ 1252, a2, 0x00000000
	rori	a2, a0, 1
	CHECK_EQ 1253, a2, 0x80000000
	slli	a2, a0, 1
	CHECK_EQ 1254, a2, 0x00000002
	srai	a2, a0, 1
	CHECK_EQ 1255, a2, 0x00000000
	li	a0, 0x00000001
	li	a1, 4
	ror	a2, a0, a1
	CHECK_EQ 1256, a2, 0x10000000
	rol	a2, a0, a1
	CHECK_EQ 1257, a2, 0x00000010
	sll	a2, a0, a1
	CHECK_EQ 1258, a2, 0x00000010
	srl	a2, a0, a1
	CHECK_EQ 1259, a2, 0x00000000
	sra	a2, a0, a1
	CHECK_EQ 1260, a2, 0x00000000
	rori	a2, a0, 4
	CHECK_EQ 1261, a2, 0x10000000
	slli	a2, a0, 4
	CHECK_EQ 1262, a2, 0x00000010
	srai	a2, a0, 4
	CHECK_EQ 1263, a2, 0x00000000
	li	a0, 0x00000001
	li	a1, 8
	ror	a2, a0, a1
	CHECK_EQ 1264, a2, 0x01000000
	rol	a2, a0, a1
	CHECK_EQ 1265, a2, 0x00000100
	sll	a2, a0, a1
	CHECK_EQ 1266, a2, 0x00000100
	srl	a2, a0, a1
	CHECK_EQ 1267, a2, 0x00000000
	sra	a2, a0, a1
	CHECK_EQ 1268, a2, 0x00000000
	rori	a2, a0, 8
	CHECK_EQ 1269, a2, 0x01000000
	slli	a2, a0, 8
	CHECK_EQ 1270, a2, 0x00000100
	srai	a2, a0, 8
	CHECK_EQ 1271, a2, 0x00000000
	li	a0, 0x00000001
	li	a1, 13
	ror	a2, a0, a1
	CHECK_EQ 1272, a2, 0x00080000
	rol	a2, a0, a1
	CHECK_EQ 1273, a2, 0x00002000
	sll	a2, a0, a1
	CHECK_EQ 1274, a2, 0x00002000
	srl	a2, a0, a1
	CHECK_EQ 1275, a2, 0x00000000
	sra	a2, a0, a1
	CHECK_EQ 1276, a2, 0x00000000
	rori	a2, a0, 13
	CHECK_EQ 1277, a2, 0x00080000
	slli	a2, a0, 13
	CHECK_EQ 1278, a2, 0x00002000
	srai	a2, a0, 13
	CHECK_EQ 1279, a2, 0x00000000
	li	a0, 0x00000001
	li	a1, 16
	ror	a2, a0, a1
	CHECK_EQ 1280, a2, 0x00010000
	rol	a2, a0, a1
	CHECK_EQ 1281, a2, 0x00010000
	sll	a2, a0, a1
	CHECK_EQ 1282, a2, 0x00010000
	srl	a2, a0, a1
	CHECK_EQ 1283, a2, 0x00000000
	sra	a2, a0, a1
	CHECK_EQ 1284, a2, 0x00000000
	rori	a2, a0, 16
	CHECK_EQ 1285, a2, 0x00010000
	slli	a2, a0, 16
	CHECK_EQ 1286, a2, 0x00010000
	srai	a2, a0, 16
	CHECK_EQ 1287, a2, 0x00000000
	li	a0, 0x00000001
	li	a1, 31
	ror	a2, a0, a1
	CHECK_EQ 1288, a2, 0x00000002
	rol	a2, a0, a1
	CHECK_EQ 1289, a2, 0x80000000
	sll	a2, a0, a1
	CHECK_EQ 1290, a2, 0x80000000
	srl	a2, a0, a1
	CHECK_EQ 1291, a2, 0x00000000
	sra	a2, a0, a1
	CHECK_EQ 1292, a2, 0x00000000
	rori	a2, a0, 31
	CHECK_EQ 1293, a2, 0x00000002
	slli	a2, a0, 31
	CHECK_EQ 1294, a2, 0x80000000
	srai	a2, a0, 31
	CHECK_EQ 1295, a2, 0x00000000
	li	a0, 0x00000001
	li	a1, 32
	ror	a2, a0, a1
	CHECK_EQ 1296, a2, 0x00000001
	rol	a2, a0, a1
	CHECK_EQ 1297, a2, 0x00000001
	sll	a2, a0, a1
	CHECK_EQ 1298, a2, 0x00000001
	srl	a2, a0, a1
	CHECK_EQ 1299, a2, 0x00000001
	sra	a2, a0, a1
	CHECK_EQ 1300, a2, 0x00000001
	li	a0, 0x00000001
	li	a1, 33
	ror	a2, a0, a1
	CHECK_EQ 1304, a2, 0x80000000
	rol	a2, a0, a1
	CHECK_EQ 1305, a2, 0x00000002
	sll	a2, a0, a1
	CHECK_EQ 1306, a2, 0x00000002
	srl	a2, a0, a1
	CHECK_EQ 1307, a2, 0x00000000
	sra	a2, a0, a1
	CHECK_EQ 1308, a2, 0x00000000
	li	a0, 0x00000001
	li	a1, 63
	ror	a2, a0, a1
	CHECK_EQ 1312, a2, 0x00000002
	rol	a2, a0, a1
	CHECK_EQ 1313, a2, 0x80000000
	sll	a2, a0, a1
	CHECK_EQ 1314, a2, 0x80000000
	srl	a2, a0, a1
	CHECK_EQ 1315, a2, 0x00000000
	sra	a2, a0, a1
	CHECK_EQ 1316, a2, 0x00000000
	li	a0, 0xffffffff
	li	a1, 0
	ror	a2, a0, a1
	CHECK_EQ 1320, a2, 0xffffffff
	rol	a2, a0, a1
	CHECK_EQ 1321, a2, 0xffffffff
	sll	a2, a0, a1
	CHECK_EQ 1322, a2, 0xffffffff
	srl	a2, a0, a1
	CHECK_EQ 1323, a2, 0xffffffff
	sra	a2, a0, a1
	CHECK_EQ 1324, a2, 0xffffffff
	rori	a2, a0, 0
	CHECK_EQ 1325, a2, 0xffffffff
	slli	a2, a0, 0
	CHECK_EQ 1326, a2, 0xffffffff
	srai	a2, a0, 0
	CHECK_EQ 1327, a2, 0xffffffff
	li	a0, 0xffffffff
	li	a1, 1
	ror	a2, a0, a1
	CHECK_EQ 1328, a2, 0xffffffff
	rol	a2, a0, a1
	CHECK_EQ 1329, a2, 0xffffffff
	sll	a2, a0, a1
	CHECK_EQ 1330, a2, 0xfffffffe
	srl	a2, a0, a1
	CHECK_EQ 1331, a2, 0x7fffffff
	sra	a2, a0, a1
	CHECK_EQ 1332, a2, 0xffffffff
	rori	a2, a0, 1
	CHECK_EQ 1333, a2, 0xffffffff
	slli	a2, a0, 1
	CHECK_EQ 1334, a2, 0xfffffffe
	srai	a2, a0, 1
	CHECK_EQ 1335, a2, 0xffffffff
	li	a0, 0xffffffff
	li	a1, 4
	ror	a2, a0, a1
	CHECK_EQ 1336, a2, 0xffffffff
	rol	a2, a0, a1
	CHECK_EQ 1337, a2, 0xffffffff
	sll	a2, a0, a1
	CHECK_EQ 1338, a2, 0xfffffff0
	srl	a2, a0, a1
	CHECK_EQ 1339, a2, 0x0fffffff
	sra	a2, a0, a1
	CHECK_EQ 1340, a2, 0xffffffff
	rori	a2, a0, 4
	CHECK_EQ 1341, a2, 0xffffffff
	slli	a2, a0, 4
	CHECK_EQ 1342, a2, 0xfffffff0
	srai	a2, a0, 4
	CHECK_EQ 1343, a2, 0xffffffff
	li	a0, 0xffffffff
	li	a1, 8
	ror	a2, a0, a1
	CHECK_EQ 1344, a2, 0xffffffff
	rol	a2, a0, a1
	CHECK_EQ 1345, a2, 0xffffffff
	sll	a2, a0, a1
	CHECK_EQ 1346, a2, 0xffffff00
	srl	a2, a0, a1
	CHECK_EQ 1347, a2, 0x00ffffff
	sra	a2, a0, a1
	CHECK_EQ 1348, a2, 0xffffffff
	rori	a2, a0, 8
	CHECK_EQ 1349, a2, 0xffffffff
	slli	a2, a0, 8
	CHECK_EQ 1350, a2, 0xffffff00
	srai	a2, a0, 8
	CHECK_EQ 1351, a2, 0xffffffff
	li	a0, 0xffffffff
	li	a1, 13
	ror	a2, a0, a1
	CHECK_EQ 1352, a2, 0xffffffff
	rol	a2, a0, a1
	CHECK_EQ 1353, a2, 0xffffffff
	sll	a2, a0, a1
	CHECK_EQ 1354, a2, 0xffffe000
	srl	a2, a0, a1
	CHECK_EQ 1355, a2, 0x0007ffff
	sra	a2, a0, a1
	CHECK_EQ 1356, a2, 0xffffffff
	rori	a2, a0, 13
	CHECK_EQ 1357, a2, 0xffffffff
	slli	a2, a0, 13
	CHECK_EQ 1358, a2, 0xffffe000
	srai	a2, a0, 13
	CHECK_EQ 1359, a2, 0xffffffff
	li	a0, 0xffffffff
	li	a1, 16
	ror	a2, a0, a1
	CHECK_EQ 1360, a2, 0xffffffff
	rol	a2, a0, a1
	CHECK_EQ 1361, a2, 0xffffffff
	sll	a2, a0, a1
	CHECK_EQ 1362, a2, 0xffff0000
	srl	a2, a0, a1
	CHECK_EQ 1363, a2, 0x0000ffff
	sra	a2, a0, a1
	CHECK_EQ 1364, a2, 0xffffffff
	rori	a2, a0, 16
	CHECK_EQ 1365, a2, 0xffffffff
	slli	a2, a0, 16
	CHECK_EQ 1366, a2, 0xffff0000
	srai	a2, a0, 16
	CHECK_EQ 1367, a2, 0xffffffff
	li	a0, 0xffffffff
	li	a1, 31
	ror	a2, a0, a1
	CHECK_EQ 1368, a2, 0xffffffff
	rol	a2, a0, a1
	CHECK_EQ 1369, a2, 0xffffffff
	sll	a2, a0, a1
	CHECK_EQ 1370, a2, 0x80000000
	srl	a2, a0, a1
	CHECK_EQ 1371, a2, 0x00000001
	sra	a2, a0, a1
	CHECK_EQ 1372, a2, 0xffffffff
	rori	a2, a0, 31
	CHECK_EQ 1373, a2, 0xffffffff
	slli	a2, a0, 31
	CHECK_EQ 1374, a2, 0x80000000
	srai	a2, a0, 31
	CHECK_EQ 1375, a2, 0xffffffff
	li	a0, 0xffffffff
	li	a1, 32
	ror	a2, a0, a1
	CHECK_EQ 1376, a2, 0xffffffff
	rol	a2, a0, a1
	CHECK_EQ 1377, a2, 0xffffffff
	sll	a2, a0, a1
	CHECK_EQ 1378, a2, 0xffffffff
	srl	a2, a0, a1
	CHECK_EQ 1379, a2, 0xffffffff
	sra	a2, a0, a1
	CHECK_EQ 1380, a2, 0xffffffff
	li	a0, 0xffffffff
	li	a1, 33
	ror	a2, a0, a1
	CHECK_EQ 1384, a2, 0xffffffff
	rol	a2, a0, a1
	CHECK_EQ 1385, a2, 0xffffffff
	sll	a2, a0, a1
	CHECK_EQ 1386, a2, 0xfffffffe
	srl	a2, a0, a1
	CHECK_EQ 1387, a2, 0x7fffffff
	sra	a2, a0, a1
	CHECK_EQ 1388, a2, 0xffffffff
	li	a0, 0xffffffff
	li	a1, 63
	ror	a2, a0, a1
	CHECK_EQ 1392, a2, 0xffffffff
	rol	a2, a0, a1
	CHECK_EQ 1393, a2, 0xffffffff
	sll	a2, a0, a1
	CHECK_EQ 1394, a2, 0x80000000
	srl	a2, a0, a1
	CHECK_EQ 1395, a2, 0x00000001
	sra	a2, a0, a1
	CHECK_EQ 1396, a2, 0xffffffff
	li	a0, 0x00000000
	li	a1, 0
	ror	a2, a0, a1
	CHECK_EQ 1400, a2, 0x00000000
	rol	a2, a0, a1
	CHECK_EQ 1401, a2, 0x00000000
	sll	a2, a0, a1
	CHECK_EQ 1402, a2, 0x00000000
	srl	a2, a0, a1
	CHECK_EQ 1403, a2, 0x00000000
	sra	a2, a0, a1
	CHECK_EQ 1404, a2, 0x00000000
	rori	a2, a0, 0
	CHECK_EQ 1405, a2, 0x00000000
	slli	a2, a0, 0
	CHECK_EQ 1406, a2, 0x00000000
	srai	a2, a0, 0
	CHECK_EQ 1407, a2, 0x00000000
	li	a0, 0x00000000
	li	a1, 1
	ror	a2, a0, a1
	CHECK_EQ 1408, a2, 0x00000000
	rol	a2, a0, a1
	CHECK_EQ 1409, a2, 0x00000000
	sll	a2, a0, a1
	CHECK_EQ 1410, a2, 0x00000000
	srl	a2, a0, a1
	CHECK_EQ 1411, a2, 0x00000000
	sra	a2, a0, a1
	CHECK_EQ 1412, a2, 0x00000000
	rori	a2, a0, 1
	CHECK_EQ 1413, a2, 0x00000000
	slli	a2, a0, 1
	CHECK_EQ 1414, a2, 0x00000000
	srai	a2, a0, 1
	CHECK_EQ 1415, a2, 0x00000000
	li	a0, 0x00000000
	li	a1, 4
	ror	a2, a0, a1
	CHECK_EQ 1416, a2, 0x00000000
	rol	a2, a0, a1
	CHECK_EQ 1417, a2, 0x00000000
	sll	a2, a0, a1
	CHECK_EQ 1418, a2, 0x00000000
	srl	a2, a0, a1
	CHECK_EQ 1419, a2, 0x00000000
	sra	a2, a0, a1
	CHECK_EQ 1420, a2, 0x00000000
	rori	a2, a0, 4
	CHECK_EQ 1421, a2, 0x00000000
	slli	a2, a0, 4
	CHECK_EQ 1422, a2, 0x00000000
	srai	a2, a0, 4
	CHECK_EQ 1423, a2, 0x00000000
	li	a0, 0x00000000
	li	a1, 8
	ror	a2, a0, a1
	CHECK_EQ 1424, a2, 0x00000000
	rol	a2, a0, a1
	CHECK_EQ 1425, a2, 0x00000000
	sll	a2, a0, a1
	CHECK_EQ 1426, a2, 0x00000000
	srl	a2, a0, a1
	CHECK_EQ 1427, a2, 0x00000000
	sra	a2, a0, a1
	CHECK_EQ 1428, a2, 0x00000000
	rori	a2, a0, 8
	CHECK_EQ 1429, a2, 0x00000000
	slli	a2, a0, 8
	CHECK_EQ 1430, a2, 0x00000000
	srai	a2, a0, 8
	CHECK_EQ 1431, a2, 0x00000000
	li	a0, 0x00000000
	li	a1, 13
	ror	a2, a0, a1
	CHECK_EQ 1432, a2, 0x00000000
	rol	a2, a0, a1
	CHECK_EQ 1433, a2, 0x00000000
	sll	a2, a0, a1
	CHECK_EQ 1434, a2, 0x00000000
	srl	a2, a0, a1
	CHECK_EQ 1435, a2, 0x00000000
	sra	a2, a0, a1
	CHECK_EQ 1436, a2, 0x00000000
	rori	a2, a0, 13
	CHECK_EQ 1437, a2, 0x00000000
	slli	a2, a0, 13
	CHECK_EQ 1438, a2, 0x00000000
	srai	a2, a0, 13
	CHECK_EQ 1439, a2, 0x00000000
	li	a0, 0x00000000
	li	a1, 16
	ror	a2, a0, a1
	CHECK_EQ 1440, a2, 0x00000000
	rol	a2, a0, a1
	CHECK_EQ 1441, a2, 0x00000000
	sll	a2, a0, a1
	CHECK_EQ 1442, a2, 0x00000000
	srl	a2, a0, a1
	CHECK_EQ 1443, a2, 0x00000000
	sra	a2, a0, a1
	CHECK_EQ 1444, a2, 0x00000000
	rori	a2, a0, 16
	CHECK_EQ 1445, a2, 0x00000000
	slli	a2, a0, 16
	CHECK_EQ 1446, a2, 0x00000000
	srai	a2, a0, 16
	CHECK_EQ 1447, a2, 0x00000000
	li	a0, 0x00000000
	li	a1, 31
	ror	a2, a0, a1
	CHECK_EQ 1448, a2, 0x00000000
	rol	a2, a0, a1
	CHECK_EQ 1449, a2, 0x00000000
	sll	a2, a0, a1
	CHECK_EQ 1450, a2, 0x00000000
	srl	a2, a0, a1
	CHECK_EQ 1451, a2, 0x00000000
	sra	a2, a0, a1
	CHECK_EQ 1452, a2, 0x00000000
	rori	a2, a0, 31
	CHECK_EQ 1453, a2, 0x00000000
	slli	a2, a0, 31
	CHECK_EQ 1454, a2, 0x00000000
	srai	a2, a0, 31
	CHECK_EQ 1455, a2, 0x00000000
	li	a0, 0x00000000
	li	a1, 32
	ror	a2, a0, a1
	CHECK_EQ 1456, a2, 0x00000000
	rol	a2, a0, a1
	CHECK_EQ 1457, a2, 0x00000000
	sll	a2, a0, a1
	CHECK_EQ 1458, a2, 0x00000000
	srl	a2, a0, a1
	CHECK_EQ 1459, a2, 0x00000000
	sra	a2, a0, a1
	CHECK_EQ 1460, a2, 0x00000000
	li	a0, 0x00000000
	li	a1, 33
	ror	a2, a0, a1
	CHECK_EQ 1464, a2, 0x00000000
	rol	a2, a0, a1
	CHECK_EQ 1465, a2, 0x00000000
	sll	a2, a0, a1
	CHECK_EQ 1466, a2, 0x00000000
	srl	a2, a0, a1
	CHECK_EQ 1467, a2, 0x00000000
	sra	a2, a0, a1
	CHECK_EQ 1468, a2, 0x00000000
	li	a0, 0x00000000
	li	a1, 63
	ror	a2, a0, a1
	CHECK_EQ 1472, a2, 0x00000000
	rol	a2, a0, a1
	CHECK_EQ 1473, a2, 0x00000000
	sll	a2, a0, a1
	CHECK_EQ 1474, a2, 0x00000000
	srl	a2, a0, a1
	CHECK_EQ 1475, a2, 0x00000000
	sra	a2, a0, a1
	CHECK_EQ 1476, a2, 0x00000000
	li	a0, 0x0f0f00ff
	ror	a0, a0, a0
	CHECK_EQ 1480, a0, 0x1e1e01fe
	rori	a1, a0, 7
	rol	a1, a1, a1
	CHECK_EQ 1481, a1, 0xe1e1e01f
	TEST_PASS

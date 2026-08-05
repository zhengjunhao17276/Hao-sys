# create_disk.ps1 - 创建 disk.img（FAT16，含 SHELL.BIN）
# PowerShell 脚本，无需额外工具

$shell = [byte[]][IO.File]::ReadAllBytes("$PSScriptRoot\user\shell.bin")
$disk = New-Object byte[] (32*1024*1024)  # 32MB

$bps=512; $spc=2; $res=1; $nf=2; $rec=512; $fat_sz=128
$total=$disk.Length/$bps

# Boot sector + BPB
$disk[0]=0xEB;$disk[1]=0x3C;$disk[2]=0x90
$o=[byte[]][Text.Encoding]::ASCII.GetBytes("HaoOS   ");0..7|%{$disk[3+$_]=$o[$_]}
$disk[11]=($bps-band0xFF);$disk[12]=($bps-shr8)-band0xFF
$disk[13]=$spc
$disk[14]=($res-band0xFF);$disk[15]=($res-shr8)-band0xFF
$disk[16]=$nf
$disk[17]=($rec-band0xFF);$disk[18]=($rec-shr8)-band0xFF
$disk[19]=($total-band0xFF);$disk[20]=($total-shr8)-band0xFF
$disk[21]=0xF8
$disk[22]=($fat_sz-band0xFF);$disk[23]=($fat_sz-shr8)-band0xFF
$disk[24]=63;$disk[25]=0;$disk[26]=16;$disk[27]=0
$disk[32]=($total-band0xFF);$disk[33]=($total-shr8)-band0xFF
$disk[34]=($total-shr16)-band0xFF;$disk[35]=($total-shr24)-band0xFF
$disk[36]=0x80;$disk[38]=0x29
$disk[39]=0xDE;$disk[40]=0xAD;$disk[41]=0xBE;$disk[42]=0xEF
$vl=[byte[]][Text.Encoding]::ASCII.GetBytes("HAOOS      ");0..10|%{$disk[43+$_]=$vl[$_]}
$fs=[byte[]][Text.Encoding]::ASCII.GetBytes("FAT16   ");0..7|%{$disk[54+$_]=$fs[$_]}
$disk[510]=0x55;$disk[511]=0xAA

# FAT #1
$f1=512;$disk[$f1]=0xF8;$disk[$f1+1]=0xFF;$disk[$f1+2]=0xFF;$disk[$f1+3]=0xFF
$nc=[int][math]::Ceiling($shell.Length/($spc*$bps))
0..($nc-1)|%{$cl=2+$_;$nxt=if($_-lt$nc-1){$cl+1}else{0xFFF8};$p=$f1+$cl*2;$disk[$p]=$nxt-band0xFF;$disk[$p+1]=($nxt-shr8)-band0xFF}

# FAT #2
0..(128*512-1)|%{$disk[129*512+$_]=$disk[$f1+$_]}

# Root dir
$ro=257*512
$n=[byte[]][Text.Encoding]::ASCII.GetBytes("SHELL   BIN");0..10|%{$disk[$ro+$_]=$n[$_]}
$disk[$ro+11]=0x20;$disk[$ro+26]=2;$disk[$ro+27]=0
$disk[$ro+28]=($shell.Length-band0xFF);$disk[$ro+29]=($shell.Length-shr8)-band0xFF
$disk[$ro+30]=($shell.Length-shr16)-band0xFF;$disk[$ro+31]=($shell.Length-shr24)-band0xFF

# Shell data
0..($shell.Length-1)|%{$disk[289*512+$_]=$shell[$_]}

[IO.File]::WriteAllBytes("$PSScriptRoot\disk.img", $disk)
Write-Output "disk.img created: $($disk.Length/1MB) MB"

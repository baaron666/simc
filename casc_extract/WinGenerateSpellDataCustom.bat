call py -3 casc_extract.py -m batch --cdn --custom custom_build_config -o wow
cd wow
dir /b /a:D /O:-D>hi.txt
set /p wowdir=<hi.txt
del hi.txt
cd ..
cd ..
set curr=%cd%
cd dbc_extract3
call generate.bat %wowdir% "%curr%\casc_extract\wow"
pause

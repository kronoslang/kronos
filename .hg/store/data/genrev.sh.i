         `   �      ����������.�\��Qa�6�4����[!�            (�/� �� �#!/bin/bash
echo "#define KRONOS_HG_REVISION $(hg id -n)"BRANCHbTAGSt)" Nm؅j+*m     `     g   �      �    ���� $֍����A��_>�Ź��            (�/� �� �      �   �echo "#define KRONOS_HG_REVISION \"$(hg id -n)\""
BRANCHbTAGSt)\"" Rq�KWނ

     �     _   �     
   ����.2�M�ҊݏX��n�g��               q   �   Secho "#define KRONOS_HG_TAGS \"$(hg parent -T {latesttag}~{latesttagdistance-2})\""    &     _   �        ����71�c��~]�bb5G�g�               q   �   Secho "#define KRONOS_HG_TAGS \"$(hg parent -T {latesttag}~{latesttagdistance-1})\""
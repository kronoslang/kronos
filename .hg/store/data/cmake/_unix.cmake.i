         �   �      q���������}1+��3C�?-�%l+��            (�/� �E �H@Af;I&Z��h���c���Jm)ј��V&�UK�7�%F
'���+;���ONI����c�,�<��&�9��@��y 0�g���6'
����,�Ӆ��^[���U�WX32�\������O[ 3#��     �     �   �     �    ����Y�) ���B7�t5 ���H�            u# detect debian-ish platform
find_program(DPKG dpkg)
if (NOT ${DPKG} STREQUAL "DPKG-NOTFOUND")
	setDefaultPackageGenerator(BINARY_DEB)
endif()
#include "IEnumerable.h"
#include <iostream>
#include <vector>
#include <sstream>
#include <algorithm>
using namespace std;
using namespace QueryXx;

LAZY_ENUMERATOR(int,CountingSequence)
{
	int count;
	LAZY_BEGIN
		count=1;
		while(true) LAZY_YIELD(count++);
	LAZY_END
};

LAZY_ENUMERATOR(int,FibonacciNumbers)
{
	int a,b,tmp;
	LAZY_BEGIN
		a = 0;
		b = 1;
		while(true)
		{
			LAZY_YIELD(b);
			tmp = a;
			a = b;
			b += tmp;
		}
	LAZY_END
};

bool IsOdd(int x) {return (x%2)==1;}

double Add(double x,double y) {return x+y;}

int main()
{
	double matrix[10][10];
	for(int i(0);i<10;i++)
		for(int j(0);j<10;j++)
			matrix[i][j] = sin(i) * cos(j);

}
#define stride 201326592
#define line 64
#define nway 8
int main(){
	volatile char buf[line * stride * 2 * nway];
	for(int i = 0; i< 0x0ffffff0; i++){
		int j = line * stride * (i%(2*nway));
		buf[j] += 1;
	}
	for(int i = 0; i< 0x0ffffff0; i+=2){
		int j = line * stride * (i%(2*nway));
		int k = line * stride * (i+1%(2*nway));

		buf[j] += 1;
		buf[k] += 1;
	}
	return 0;
}

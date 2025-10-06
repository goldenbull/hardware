import PIL.Image as Image

im = Image.open("my-heart.bmp")

myfile = open("c/src/ImageData_my_heart.c", "wt")

myfile.write("#include \"ImageData.h\"\n")
myfile.write("/* Image Width:%d Height:%d */" % (im.size[0], im.size[1]))
myfile.write("\n")
myfile.write("const unsigned char gImg_my_heart[]={")

pix = im.load()

# w is the horizontal axis of the pixels' value of the picture
# h is the vertical axis of the pixels' value of the picture
for h in range(im.height):
    myfile.write("\n")
    for w in range(im.width):
        if w < im.size[0]:
            R = pix[w, h][0] >> 3
            G = pix[w, h][1] >> 2
            B = pix[w, h][2] >> 3

            # rgb = (R << 11) | (G << 5) | B
            # myfile.write("0x%x," % (rgb))

            b1 = (R << 3) | (G >> 3)
            b2 = ((G & 0x07) << 5) | (B)
            myfile.write("0x%x,0x%x," % (b2, b1))  # little endian
        else:
            rgb = 0
            # myfile.write("0x0,")
            myfile.write("0x%x,0x%x," % (0, 0))

myfile.write("};")
myfile.close()

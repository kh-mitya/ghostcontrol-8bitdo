# Draws assets/icon0.png: python3 tools/make_icon.py /path/to/DejaVuSans-Bold.ttf assets/icon0.png
from PIL import Image, ImageDraw, ImageFont
import sys
W=512
img=Image.new('RGB',(W,W))
d=ImageDraw.Draw(img)
for y in range(W):  # dark navy → teal gradient
    t=y/W
    d.line([(0,y),(W,y)],fill=(int(12+10*t),int(18+60*t),int(40+70*t)))
font=sys.argv[1]
# controller silhouette
cx,cy=256,230
d.rounded_rectangle([96,150,416,310],radius=80,fill=(235,240,245))
d.ellipse([86,200,206,360],fill=(235,240,245)); d.ellipse([306,200,426,360],fill=(235,240,245))
# d-pad
d.rectangle([140,214,190,234],fill=(30,40,60)); d.rectangle([155,199,175,249],fill=(30,40,60))
# face buttons
for (x,y,c) in [(360,195,(255,190,0)),(390,224,(220,50,50)),(330,224,(40,120,220)),(360,253,(40,170,70))]:
    d.ellipse([x-13,y-13,x+13,y+13],fill=c)
# sticks
d.ellipse([205,255,245,295],fill=(30,40,60)); d.ellipse([267,255,307,295],fill=(30,40,60))
f1=ImageFont.truetype(font,64); f2=ImageFont.truetype(font,40)
def ctext(y,s,f,col):
    w=d.textlength(s,font=f); d.text(((W-w)/2,y),s,font=f,fill=col)
ctext(385,"GhostControl",f1,(255,255,255))
ctext(455,"button mapper",f2,(150,220,230))

img.save(sys.argv[2],optimize=True)

/* Compiled by test_s20_controls.py with the real exposure_limits() body. */
struct event { uint16_t reg, value; unsigned width; };
static struct event events[8];
static unsigned count, fail_at;
static bool send(void *context, uint16_t reg, uint16_t value, unsigned width)
{
    (void)context;
    events[count]=(struct event){reg,value,width};
    return count++ != fail_at;
}
static bool send16(void *c,uint16_t r,uint16_t v) { return send(c,r,v,2); }
static bool send8(void *c,uint16_t r,uint8_t v) { return send(c,r,v,1); }

int main(void)
{
    const struct redwood_2ld_control_io io={NULL,send16,send8};
    const struct event expected[]={ {0xfcfc,0x4000,2}, {0x0104,1,1}, {0x0340,22564,2},
        {0x0202,1000,2}, {0x0204,64,2}, {0x0104,0,1} };
    uint16_t failed=0xffff;
    count=0; fail_at=99;
    assert(redwood_2ld_write_exposure(&io,1000,64,22564,&failed));
    assert(failed==0 && count==6);
    for(unsigned i=0;i<6;i++) assert(events[i].reg==expected[i].reg && events[i].value==expected[i].value && events[i].width==expected[i].width);
    for(unsigned i=0;i<6;i++) {
        count=0;fail_at=i;
        assert(!redwood_2ld_write_exposure(&io,1000,64,22564,&failed));
        assert(failed==expected[i].reg);
        assert(count==(i==0 ? 1 : i==5 ? 6 : i+2));
        if(i>0) assert(events[count-1].reg==0x0104 && events[count-1].value==0);
    }
    count=0;fail_at=99;
    assert(redwood_2ld_write_exposure(&io,1000,64,0,&failed));
    assert(count==5 && events[2].reg==0x0202);
    /* Samsung USE_GROUP_PARAM_HOLD=0 path: exactly four writes and no hold register. */
    count=0;fail_at=99;
    assert(redwood_2ld_write_exposure_mode(&io,false,1000,64,22564,&failed));
    assert(count==4 && events[0].reg==0xfcfc && events[1].reg==0x0340 &&
        events[2].reg==0x0202 && events[3].reg==0x0204);
    for(unsigned i=0;i<4;i++) {
        count=0;fail_at=i;
        assert(!redwood_2ld_write_exposure_mode(&io,false,1000,64,22564,&failed));
        assert(count==i+1 && failed==events[i].reg);
        for(unsigned j=0;j<count;j++) assert(events[j].reg!=0x0104);
    }
    assert(redwood_2ld_line_ns(2272)==1468 && redwood_2ld_line_ns(4240)==2740);

    mode.frame_length=22564;
    s20_real=true;s20_rear_now=true;
    uint32_t l=1001,g=0,f=0;
    exposure_limits(&l,&g,&f);
    assert(l==1000 && g==32 && f==22564);
    l=65535;g=0xffff;f=65519;
    exposure_limits(&l,&g,&f);
    assert(l==65478 && g==512 && f==65519); /* round AFTER odd frame clamp */
    l=1;g=32;f=0;
    exposure_limits(&l,&g,&f); assert(l==16);
    l=UINT32_MAX;g=32;f=0;
    exposure_limits(&l,&g,&f); assert(l==65480 && f==65520);
    for(uint32_t requested=0;requested<66000;requested++) {
        l=requested;g=32;f=0;exposure_limits(&l,&g,&f);
        assert(l>=16 && !(l&1) && l<=f-40 && f>=22564 && f<=65520);
    }
    s20_rear_now=false;
    l=1001;g=32;f=0;exposure_limits(&l,&g,&f);assert(l==1001);
    s20_real=false;
    l=1;g=32;f=0;exposure_limits(&l,&g,&f);assert(l==4);
    puts("rear controls: each I2C failure, hold release, timing, 66000 exposure limits and front preservation PASS");
    return 0;
}

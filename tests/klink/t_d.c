/* второе определение общего символа cm (для -fcommon); при KL_EXTERN_CM - внешняя ссылка (kcc не делает общих символов) */
#ifdef KL_EXTERN_CM
extern int cm;
#else
int cm;
#endif
int cm_user2(void) { cm += 7; return cm; }
